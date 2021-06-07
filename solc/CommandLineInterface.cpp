/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * @author Lefteris <lefteris@ethdev.com>
 * @author Gav Wood <g@ethdev.com>
 * @date 2014
 * Solidity command line interface.
 */
#include <solc/CommandLineInterface.h>

#include "solidity/BuildInfo.h"

#include <libsolidity/interface/Version.h>
#include <libsolidity/ast/ASTJsonConverter.h>
#include <libsolidity/ast/ASTJsonImporter.h>
#include <libsolidity/analysis/NameAndTypeResolver.h>
#include <libsolidity/interface/CompilerStack.h>
#include <libsolidity/interface/StandardCompiler.h>
#include <libsolidity/interface/GasEstimator.h>
#include <libsolidity/interface/DebugSettings.h>
#include <libsolidity/interface/ImportRemapper.h>
#include <libsolidity/interface/StorageLayout.h>

#include <libyul/AssemblyStack.h>

#include <libevmasm/Instruction.h>
#include <libevmasm/GasMeter.h>

#include <liblangutil/Exceptions.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/SourceReferenceFormatter.h>

#include <libsmtutil/Exceptions.h>

#include <libsolutil/Common.h>
#include <libsolutil/CommonData.h>
#include <libsolutil/CommonIO.h>
#include <libsolutil/JSON.h>

#include <algorithm>
#include <memory>

#include <range/v3/view/map.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/algorithm/string.hpp>

#ifdef _WIN32 // windows
	#include <io.h>
	#define isatty _isatty
	#define fileno _fileno
#else // unix
	#include <unistd.h>
#endif

#include <iostream>
#include <fstream>

#if !defined(STDERR_FILENO)
	#define STDERR_FILENO 2
#endif


using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::langutil;

namespace solidity::frontend
{

namespace
{

static bool g_hasOutput = false;

std::ostream& sout(bool _used = true)
{
	if (_used)
		g_hasOutput = true;
	return cout;
}

std::ostream& serr(bool _used = true)
{
	if (_used)
		g_hasOutput = true;
	return cerr;
}

}

#define cout
#define cerr

static string const g_stdinFileName = "<stdin>";
static string const g_strAbi = "abi";
static string const g_strAsm = "asm";
static string const g_strAst = "ast";
static string const g_strBinary = "bin";
static string const g_strBinaryRuntime = "bin-runtime";
static string const g_strContracts = "contracts";
static string const g_strFunDebug = "function-debug";
static string const g_strFunDebugRuntime = "function-debug-runtime";
static string const g_strGeneratedSources = "generated-sources";
static string const g_strGeneratedSourcesRuntime = "generated-sources-runtime";
static string const g_strNatspecDev = "devdoc";
static string const g_strNatspecUser = "userdoc";
static string const g_strOpcodes = "opcodes";
static string const g_strSignatureHashes = "hashes";
static string const g_strSourceList = "sourceList";
static string const g_strSources = "sources";
static string const g_strSrcMap = "srcmap";
static string const g_strSrcMapRuntime = "srcmap-runtime";
static string const g_strStorageLayout = "storage-layout";
static string const g_strVersion = "version";

static bool needsHumanTargetedStdout(CommandLineOptions const& _options)
{
	if (_options.estimateGas)
		return true;
	if (!_options.outputDir.empty())
		return false;
	return
		_options.selectedOutputs.abi ||
		_options.selectedOutputs.asm_ ||
		_options.selectedOutputs.asmJson ||
		_options.selectedOutputs.binary ||
		_options.selectedOutputs.binaryRuntime ||
		_options.selectedOutputs.metadata ||
		_options.selectedOutputs.natspecUser ||
		_options.selectedOutputs.natspecDev ||
		_options.selectedOutputs.opcodes ||
		_options.selectedOutputs.signatureHashes ||
		_options.selectedOutputs.storageLayout;
}

static bool coloredOutput(CommandLineOptions const& _options)
{
	return (!_options.coloredOutput.has_value() && isatty(STDERR_FILENO)) ||
		(_options.coloredOutput.has_value() && _options.coloredOutput.value());
}

void CommandLineInterface::handleBinary(string const& _contract)
{
	if (m_options.selectedOutputs.binary)
	{
		if (!m_options.outputDir.empty())
			createFile(m_compiler->filesystemFriendlyName(_contract) + ".bin", objectWithLinkRefsHex(m_compiler->object(_contract)));
		else
		{
			sout() << "Binary:" << endl;
			sout() << objectWithLinkRefsHex(m_compiler->object(_contract)) << endl;
		}
	}
	if (m_options.selectedOutputs.binaryRuntime)
	{
		if (!m_options.outputDir.empty())
			createFile(m_compiler->filesystemFriendlyName(_contract) + ".bin-runtime", objectWithLinkRefsHex(m_compiler->runtimeObject(_contract)));
		else
		{
			sout() << "Binary of the runtime part:" << endl;
			sout() << objectWithLinkRefsHex(m_compiler->runtimeObject(_contract)) << endl;
		}
	}
}

void CommandLineInterface::handleOpcode(string const& _contract)
{
	if (!m_options.outputDir.empty())
		createFile(m_compiler->filesystemFriendlyName(_contract) + ".opcode", evmasm::disassemble(m_compiler->object(_contract).bytecode));
	else
	{
		sout() << "Opcodes:" << endl;
		sout() << std::uppercase << evmasm::disassemble(m_compiler->object(_contract).bytecode);
		sout() << endl;
	}
}

void CommandLineInterface::handleIR(string const& _contractName)
{
	if (!m_options.selectedOutputs.ir)
		return;

	if (!m_options.outputDir.empty())
		createFile(m_compiler->filesystemFriendlyName(_contractName) + ".yul", m_compiler->yulIR(_contractName));
	else
	{
		sout() << "IR:" << endl;
		sout() << m_compiler->yulIR(_contractName) << endl;
	}
}

void CommandLineInterface::handleIROptimized(string const& _contractName)
{
	if (!m_options.selectedOutputs.irOptimized)
		return;

	if (!m_options.outputDir.empty())
		createFile(m_compiler->filesystemFriendlyName(_contractName) + "_opt.yul", m_compiler->yulIROptimized(_contractName));
	else
	{
		sout() << "Optimized IR:" << endl;
		sout() << m_compiler->yulIROptimized(_contractName) << endl;
	}
}

void CommandLineInterface::handleEwasm(string const& _contractName)
{
	if (!m_options.selectedOutputs.ewasm)
		return;

	if (!m_options.outputDir.empty())
	{
		createFile(m_compiler->filesystemFriendlyName(_contractName) + ".wast", m_compiler->ewasm(_contractName));
		createFile(
			m_compiler->filesystemFriendlyName(_contractName) + ".wasm",
			asString(m_compiler->ewasmObject(_contractName).bytecode)
		);
	}
	else
	{
		sout() << "Ewasm text:" << endl;
		sout() << m_compiler->ewasm(_contractName) << endl;
		sout() << "Ewasm binary (hex): " << m_compiler->ewasmObject(_contractName).toHex() << endl;
	}
}

void CommandLineInterface::handleBytecode(string const& _contract)
{
	if (m_options.selectedOutputs.opcodes)
		handleOpcode(_contract);
	if (m_options.selectedOutputs.binary || m_options.selectedOutputs.binaryRuntime)
		handleBinary(_contract);
}

void CommandLineInterface::handleSignatureHashes(string const& _contract)
{
	if (!m_options.selectedOutputs.signatureHashes)
		return;

	Json::Value methodIdentifiers = m_compiler->methodIdentifiers(_contract);
	string out;
	for (auto const& name: methodIdentifiers.getMemberNames())
		out += methodIdentifiers[name].asString() + ": " + name + "\n";

	if (!m_options.outputDir.empty())
		createFile(m_compiler->filesystemFriendlyName(_contract) + ".signatures", out);
	else
		sout() << "Function signatures:" << endl << out;
}

void CommandLineInterface::handleMetadata(string const& _contract)
{
	if (!m_options.selectedOutputs.metadata)
		return;

	string data = m_compiler->metadata(_contract);
	if (!m_options.outputDir.empty())
		createFile(m_compiler->filesystemFriendlyName(_contract) + "_meta.json", data);
	else
		sout() << "Metadata:" << endl << data << endl;
}

void CommandLineInterface::handleABI(string const& _contract)
{
	if (!m_options.selectedOutputs.abi)
		return;

	string data = jsonCompactPrint(removeNullMembers(m_compiler->contractABI(_contract)));
	if (!m_options.outputDir.empty())
		createFile(m_compiler->filesystemFriendlyName(_contract) + ".abi", data);
	else
		sout() << "Contract JSON ABI" << endl << data << endl;
}

void CommandLineInterface::handleStorageLayout(string const& _contract)
{
	if (!m_options.selectedOutputs.storageLayout)
		return;

	string data = jsonCompactPrint(removeNullMembers(m_compiler->storageLayout(_contract)));
	if (!m_options.outputDir.empty())
		createFile(m_compiler->filesystemFriendlyName(_contract) + "_storage.json", data);
	else
		sout() << "Contract Storage Layout:" << endl << data << endl;
}

void CommandLineInterface::handleNatspec(bool _natspecDev, string const& _contract)
{
	bool enabled = false;
	std::string suffix;
	std::string title;

	if (_natspecDev)
	{
		enabled = m_options.selectedOutputs.natspecDev;
		suffix = ".docdev";
		title = "Developer Documentation";
	}
	else
	{
		enabled = m_options.selectedOutputs.natspecUser;
		suffix = ".docuser";
		title = "User Documentation";
	}

	if (enabled)
	{
		std::string output = jsonPrettyPrint(
			removeNullMembers(
				_natspecDev ?
				m_compiler->natspecDev(_contract) :
				m_compiler->natspecUser(_contract)
			)
		);

		if (!m_options.outputDir.empty())
			createFile(m_compiler->filesystemFriendlyName(_contract) + suffix, output);
		else
		{
			sout() << title << endl;
			sout() << output << endl;
		}

	}
}

void CommandLineInterface::handleGasEstimation(string const& _contract)
{
	Json::Value estimates = m_compiler->gasEstimates(_contract);
	sout() << "Gas estimation:" << endl;

	if (estimates["creation"].isObject())
	{
		Json::Value creation = estimates["creation"];
		sout() << "construction:" << endl;
		sout() << "   " << creation["executionCost"].asString();
		sout() << " + " << creation["codeDepositCost"].asString();
		sout() << " = " << creation["totalCost"].asString() << endl;
	}

	if (estimates["external"].isObject())
	{
		Json::Value externalFunctions = estimates["external"];
		sout() << "external:" << endl;
		for (auto const& name: externalFunctions.getMemberNames())
		{
			if (name.empty())
				sout() << "   fallback:\t";
			else
				sout() << "   " << name << ":\t";
			sout() << externalFunctions[name].asString() << endl;
		}
	}

	if (estimates["internal"].isObject())
	{
		Json::Value internalFunctions = estimates["internal"];
		sout() << "internal:" << endl;
		for (auto const& name: internalFunctions.getMemberNames())
		{
			sout() << "   " << name << ":\t";
			sout() << internalFunctions[name].asString() << endl;
		}
	}
}

bool CommandLineInterface::readInputFilesAndConfigureFileReader()
{
	for (boost::filesystem::path const& allowedDirectory: m_options.allowedDirectories)
		m_fileReader.allowDirectory(allowedDirectory);

	for (boost::filesystem::path const& infile: m_options.sourceFilePaths)
	{
		if (!boost::filesystem::exists(infile))
		{
			if (!m_options.ignoreMissingInputFiles)
			{
				serr() << infile << " is not found." << endl;
				return false;
			}
			else
				serr() << infile << " is not found. Skipping." << endl;

			continue;
		}

		if (!boost::filesystem::is_regular_file(infile))
		{
			if (!m_options.ignoreMissingInputFiles)
			{
				serr() << infile << " is not a valid file." << endl;
				return false;
			}
			else
				serr() << infile << " is not a valid file. Skipping." << endl;

			continue;
		}

		// NOTE: we ignore the FileNotFound exception as we manually check above
		m_fileReader.setSource(infile, readFileAsString(infile.string()));
		m_fileReader.allowDirectory(boost::filesystem::path(boost::filesystem::canonical(infile).string()).remove_filename());
	}

	if (m_options.addStdin)
		m_fileReader.setSource(g_stdinFileName, readUntilEnd(cin));

	if (m_fileReader.sourceCodes().size() == 0)
	{
		serr() << "No input files given. If you wish to use the standard input please specify \"-\" explicitly." << endl;
		return false;
	}

	return true;
}

map<string, Json::Value> CommandLineInterface::parseAstFromInput()
{
	map<string, Json::Value> sourceJsons;
	map<string, string> tmpSources;

	for (SourceCode const& sourceCode: m_fileReader.sourceCodes() | ranges::views::values)
	{
		Json::Value ast;
		astAssert(jsonParseStrict(sourceCode, ast), "Input file could not be parsed to JSON");
		astAssert(ast.isMember("sources"), "Invalid Format for import-JSON: Must have 'sources'-object");

		for (auto& src: ast["sources"].getMemberNames())
		{
			std::string astKey = ast["sources"][src].isMember("ast") ? "ast" : "AST";

			astAssert(ast["sources"][src].isMember(astKey), "astkey is not member");
			astAssert(ast["sources"][src][astKey]["nodeType"].asString() == "SourceUnit",  "Top-level node should be a 'SourceUnit'");
			astAssert(sourceJsons.count(src) == 0, "All sources must have unique names");
			sourceJsons.emplace(src, move(ast["sources"][src][astKey]));
			tmpSources[src] = util::jsonCompactPrint(ast);
		}
	}

	m_fileReader.setSources(tmpSources);

	return sourceJsons;
}

void CommandLineInterface::createFile(string const& _fileName, string const& _data)
{
	namespace fs = boost::filesystem;

	solAssert(!m_options.outputDir.empty(), "");

	// NOTE: create_directories() raises an exception if the path consists solely of '.' or '..'
	// (or equivalent such as './././.'). Paths like 'a/b/.' and 'a/b/..' are fine though.
	// The simplest workaround is to use an absolute path.
	fs::create_directories(fs::absolute(m_options.outputDir));

	string pathName = (m_options.outputDir / _fileName).string();
	if (fs::exists(pathName) && !m_options.overwriteFiles)
	{
		serr() << "Refusing to overwrite existing file \"" << pathName << "\" (use --overwrite to force)." << endl;
		m_error = true;
		return;
	}
	ofstream outFile(pathName);
	outFile << _data;
	if (!outFile)
	{
		serr() << "Could not write to file \"" << pathName << "\"." << endl;
		m_error = true;
		return;
	}
}

void CommandLineInterface::createJson(string const& _fileName, string const& _json)
{
	createFile(boost::filesystem::basename(_fileName) + string(".json"), _json);
}

bool CommandLineInterface::parseArguments(int _argc, char const* const* _argv)
{
	CommandLineParser parser;
	bool success = parser.parse(_argc, _argv, isatty(fileno(stdin)));
	if (!success)
		return false;
	g_hasOutput = g_hasOutput || CommandLineParser::hasOutput();
	m_options = parser.options();

	return true;
}

bool CommandLineInterface::processInput()
{
	m_fileReader.setBasePath(m_options.basePath);

	if (m_options.basePath != "" && !boost::filesystem::is_directory(m_options.basePath))
	{
		serr() << "Base path must be a directory: \"" << m_options.basePath << "\"\n";
		return false;
	}

	if (m_options.inputMode == InputMode::StandardJson)
	{
		string input;
		if (m_options.standardJsonInputFile.empty())
			input = readUntilEnd(cin);
		else
		{
			try
			{
				input = readFileAsString(m_options.standardJsonInputFile);
			}
			catch (FileNotFound const&)
			{
				serr() << "File not found: " << m_options.standardJsonInputFile << endl;
				return false;
			}
			catch (NotAFile const&)
			{
				serr() << "Not a regular file: " << m_options.standardJsonInputFile << endl;
				return false;
			}
		}
		StandardCompiler compiler(m_fileReader.reader());
		sout() << compiler.compile(std::move(input)) << endl;
		return true;
	}

	if (!readInputFilesAndConfigureFileReader())
		return false;

	if (m_options.inputMode == InputMode::Assembler)
		return assemble(
			m_options.inputAssemblyLanguage,
			m_options.targetMachine,
			m_options.optimize,
			m_options.yulOptimiserSteps
		);

	if (m_options.inputMode == InputMode::Linker)
		return link();

	solAssert(m_options.inputMode == InputMode::Compiler || m_options.inputMode == InputMode::CompilerWithASTImport, "");
	return compile();
}

bool CommandLineInterface::compile()
{
	solAssert(m_options.inputMode == InputMode::Compiler || m_options.inputMode == InputMode::CompilerWithASTImport, "");

	m_compiler = make_unique<CompilerStack>(m_fileReader.reader());

	SourceReferenceFormatter formatter(serr(false), coloredOutput(m_options), m_options.withErrorIds);

	try
	{
		if (m_options.metadataLiteral)
			m_compiler->useMetadataLiteralSources(true);
		m_compiler->setMetadataHash(m_options.metadataHash);
		if (m_options.initializeModelChecker)
			m_compiler->setModelCheckerSettings(m_options.modelCheckerSettings);
		m_compiler->setRemappings(m_options.remappings);
		m_compiler->setLibraries(m_options.libraries);
		m_compiler->setViaIR(m_options.experimentalViaIR);
		m_compiler->setEVMVersion(m_options.evmVersion);
		m_compiler->setRevertStringBehaviour(m_options.revertStrings);
		// TODO: Perhaps we should not compile unless requested

		m_compiler->enableIRGeneration(m_options.selectedOutputs.ir || m_options.selectedOutputs.irOptimized);
		m_compiler->enableEwasmGeneration(m_options.selectedOutputs.ewasm);

		OptimiserSettings settings = m_options.optimize ? OptimiserSettings::standard() : OptimiserSettings::minimal();
		settings.expectedExecutionsPerDeployment = m_options.expectedExecutionsPerDeployment;
		if (m_options.noOptimizeYul)
			settings.runYulOptimiser = false;

		if (m_options.yulOptimiserSteps.has_value())
			settings.yulOptimiserSteps = m_options.yulOptimiserSteps.value();
		settings.optimizeStackAllocation = settings.runYulOptimiser;
		m_compiler->setOptimiserSettings(settings);

		if (m_options.inputMode == InputMode::CompilerWithASTImport)
		{
			try
			{
				m_compiler->importASTs(parseAstFromInput());

				if (!m_compiler->analyze())
				{
					for (auto const& error: m_compiler->errors())
						formatter.printErrorInformation(*error);
					astAssert(false, "Analysis of the AST failed");
				}
			}
			catch (Exception const& _exc)
			{
				serr() << string("Failed to import AST: ") << _exc.what() << endl;
				return false;
			}
		}
		else
		{
			m_compiler->setSources(m_fileReader.sourceCodes());
			if (m_options.errorRecovery)
				m_compiler->setParserErrorRecovery(true);
		}

		bool successful = m_compiler->compile(m_options.stopAfter);

		for (auto const& error: m_compiler->errors())
		{
			g_hasOutput = true;
			formatter.printErrorInformation(*error);
		}

		if (!successful)
			return m_options.errorRecovery;
	}
	catch (CompilerError const& _exception)
	{
		g_hasOutput = true;
		formatter.printExceptionInformation(_exception, "Compiler error");
		return false;
	}
	catch (InternalCompilerError const& _exception)
	{
		serr() <<
			"Internal compiler error during compilation:" <<
			endl <<
			boost::diagnostic_information(_exception);
		return false;
	}
	catch (UnimplementedFeatureError const& _exception)
	{
		serr() <<
			"Unimplemented feature:" <<
			endl <<
			boost::diagnostic_information(_exception);
		return false;
	}
	catch (smtutil::SMTLogicError const& _exception)
	{
		serr() <<
			"SMT logic error during analysis:" <<
			endl <<
			boost::diagnostic_information(_exception);
		return false;
	}
	catch (Error const& _error)
	{
		if (_error.type() == Error::Type::DocstringParsingError)
			serr() << "Documentation parsing error: " << *boost::get_error_info<errinfo_comment>(_error) << endl;
		else
		{
			g_hasOutput = true;
			formatter.printExceptionInformation(_error, _error.typeName());
		}

		return false;
	}
	catch (Exception const& _exception)
	{
		serr() << "Exception during compilation: " << boost::diagnostic_information(_exception) << endl;
		return false;
	}
	catch (std::exception const& _e)
	{
		serr() << "Unknown exception during compilation" << (
			_e.what() ? ": " + string(_e.what()) : "."
		) << endl;
		return false;
	}
	catch (...)
	{
		serr() << "Unknown exception during compilation." << endl;
		return false;
	}

	return true;
}

void CommandLineInterface::handleCombinedJSON()
{
	if (!m_options.combinedJsonRequests.has_value())
		return;

	Json::Value output(Json::objectValue);

	output[g_strVersion] = frontend::VersionString;
	vector<string> contracts = m_compiler->contractNames();

	if (!contracts.empty())
		output[g_strContracts] = Json::Value(Json::objectValue);
	for (string const& contractName: contracts)
	{
		Json::Value& contractData = output[g_strContracts][contractName] = Json::objectValue;
		if (m_options.combinedJsonRequests->abi)
			contractData[g_strAbi] = m_compiler->contractABI(contractName);
		if (m_options.combinedJsonRequests->metadata)
			contractData["metadata"] = m_compiler->metadata(contractName);
		if (m_options.combinedJsonRequests->binary && m_compiler->compilationSuccessful())
			contractData[g_strBinary] = m_compiler->object(contractName).toHex();
		if (m_options.combinedJsonRequests->binaryRuntime && m_compiler->compilationSuccessful())
			contractData[g_strBinaryRuntime] = m_compiler->runtimeObject(contractName).toHex();
		if (m_options.combinedJsonRequests->opcodes && m_compiler->compilationSuccessful())
			contractData[g_strOpcodes] = evmasm::disassemble(m_compiler->object(contractName).bytecode);
		if (m_options.combinedJsonRequests->asm_ && m_compiler->compilationSuccessful())
			contractData[g_strAsm] = m_compiler->assemblyJSON(contractName);
		if (m_options.combinedJsonRequests->storageLayout && m_compiler->compilationSuccessful())
			contractData[g_strStorageLayout] = m_compiler->storageLayout(contractName);
		if (m_options.combinedJsonRequests->generatedSources && m_compiler->compilationSuccessful())
			contractData[g_strGeneratedSources] = m_compiler->generatedSources(contractName, false);
		if (m_options.combinedJsonRequests->generatedSourcesRuntime && m_compiler->compilationSuccessful())
			contractData[g_strGeneratedSourcesRuntime] = m_compiler->generatedSources(contractName, true);
		if (m_options.combinedJsonRequests->srcMap && m_compiler->compilationSuccessful())
		{
			auto map = m_compiler->sourceMapping(contractName);
			contractData[g_strSrcMap] = map ? *map : "";
		}
		if (m_options.combinedJsonRequests->srcMapRuntime && m_compiler->compilationSuccessful())
		{
			auto map = m_compiler->runtimeSourceMapping(contractName);
			contractData[g_strSrcMapRuntime] = map ? *map : "";
		}
		if (m_options.combinedJsonRequests->funDebug && m_compiler->compilationSuccessful())
			contractData[g_strFunDebug] = StandardCompiler::formatFunctionDebugData(
				m_compiler->object(contractName).functionDebugData
			);
		if (m_options.combinedJsonRequests->funDebugRuntime && m_compiler->compilationSuccessful())
			contractData[g_strFunDebugRuntime] = StandardCompiler::formatFunctionDebugData(
				m_compiler->runtimeObject(contractName).functionDebugData
			);
		if (m_options.combinedJsonRequests->signatureHashes)
			contractData[g_strSignatureHashes] = m_compiler->methodIdentifiers(contractName);
		if (m_options.combinedJsonRequests->natspecDev)
			contractData[g_strNatspecDev] = m_compiler->natspecDev(contractName);
		if (m_options.combinedJsonRequests->natspecUser)
			contractData[g_strNatspecUser] = m_compiler->natspecUser(contractName);
	}

	bool needsSourceList =
		m_options.combinedJsonRequests->ast ||
		m_options.combinedJsonRequests->srcMap ||
		m_options.combinedJsonRequests->srcMapRuntime;
	if (needsSourceList)
	{
		// Indices into this array are used to abbreviate source names in source locations.
		output[g_strSourceList] = Json::Value(Json::arrayValue);

		for (auto const& source: m_compiler->sourceNames())
			output[g_strSourceList].append(source);
	}

	if (m_options.combinedJsonRequests->ast)
	{
		output[g_strSources] = Json::Value(Json::objectValue);
		for (auto const& sourceCode: m_fileReader.sourceCodes())
		{
			ASTJsonConverter converter(m_compiler->state(), m_compiler->sourceIndices());
			output[g_strSources][sourceCode.first] = Json::Value(Json::objectValue);
			output[g_strSources][sourceCode.first]["AST"] = converter.toJson(m_compiler->ast(sourceCode.first));
		}
	}

	string json = m_options.prettyJson ? jsonPrettyPrint(removeNullMembers(std::move(output))) :
		jsonCompactPrint(removeNullMembers(std::move(output)));

	if (!m_options.outputDir.empty())
		createJson("combined", json);
	else
		sout() << json << endl;
}

void CommandLineInterface::handleAst()
{
	if (!m_options.selectedOutputs.astCompactJson)
		return;

	vector<ASTNode const*> asts;
	for (auto const& sourceCode: m_fileReader.sourceCodes())
		asts.push_back(&m_compiler->ast(sourceCode.first));

	if (!m_options.outputDir.empty())
	{
		for (auto const& sourceCode: m_fileReader.sourceCodes())
		{
			stringstream data;
			string postfix = "";
			ASTJsonConverter(m_compiler->state(), m_compiler->sourceIndices()).print(data, m_compiler->ast(sourceCode.first));
			postfix += "_json";
			boost::filesystem::path path(sourceCode.first);
			createFile(path.filename().string() + postfix + ".ast", data.str());
		}
	}
	else
	{
		sout() << "JSON AST (compact format):" << endl << endl;
		for (auto const& sourceCode: m_fileReader.sourceCodes())
		{
			sout() << endl << "======= " << sourceCode.first << " =======" << endl;
			ASTJsonConverter(m_compiler->state(), m_compiler->sourceIndices()).print(sout(), m_compiler->ast(sourceCode.first));
		}
	}
}

bool CommandLineInterface::actOnInput()
{
	if (m_options.inputMode == InputMode::StandardJson || m_options.inputMode == InputMode::Assembler)
		// Already done in "processInput" phase.
		return true;
	else if (m_options.inputMode == InputMode::Linker)
		writeLinkedFiles();
	else
	{
		solAssert(m_options.inputMode == InputMode::Compiler || m_options.inputMode == InputMode::CompilerWithASTImport, "");
		outputCompilationResults();
	}
	return !m_error;
}

bool CommandLineInterface::link()
{
	// Map from how the libraries will be named inside the bytecode to their addresses.
	map<string, h160> librariesReplacements;
	int const placeholderSize = 40; // 20 bytes or 40 hex characters
	for (auto const& library: m_options.libraries)
	{
		string const& name = library.first;
		// Library placeholders are 40 hex digits (20 bytes) that start and end with '__'.
		// This leaves 36 characters for the library identifier. The identifier used to
		// be just the cropped or '_'-padded library name, but this changed to
		// the cropped hex representation of the hash of the library name.
		// We support both ways of linking here.
		librariesReplacements["__" + evmasm::LinkerObject::libraryPlaceholder(name) + "__"] = library.second;

		string replacement = "__";
		for (size_t i = 0; i < placeholderSize - 4; ++i)
			replacement.push_back(i < name.size() ? name[i] : '_');
		replacement += "__";
		librariesReplacements[replacement] = library.second;
	}

	FileReader::StringMap sourceCodes = m_fileReader.sourceCodes();
	for (auto& src: sourceCodes)
	{
		auto end = src.second.end();
		for (auto it = src.second.begin(); it != end;)
		{
			while (it != end && *it != '_') ++it;
			if (it == end) break;
			if (
				end - it < placeholderSize ||
				*(it + 1) != '_' ||
				*(it + placeholderSize - 2) != '_' ||
				*(it + placeholderSize - 1) != '_'
			)
			{
				serr() << "Error in binary object file " << src.first << " at position " << (it - src.second.begin()) << endl;
				serr() << '"' << string(it, it + min(placeholderSize, static_cast<int>(end - it))) << "\" is not a valid link reference." << endl;
				return false;
			}

			string foundPlaceholder(it, it + placeholderSize);
			if (librariesReplacements.count(foundPlaceholder))
			{
				string hexStr(toHex(librariesReplacements.at(foundPlaceholder).asBytes()));
				copy(hexStr.begin(), hexStr.end(), it);
			}
			else
				serr() << "Reference \"" << foundPlaceholder << "\" in file \"" << src.first << "\" still unresolved." << endl;
			it += placeholderSize;
		}
		// Remove hints for resolved libraries.
		for (auto const& library: m_options.libraries)
			boost::algorithm::erase_all(src.second, "\n" + libraryPlaceholderHint(library.first));
		while (!src.second.empty() && *prev(src.second.end()) == '\n')
			src.second.resize(src.second.size() - 1);
	}
	m_fileReader.setSources(move(sourceCodes));

	return true;
}

void CommandLineInterface::writeLinkedFiles()
{
	for (auto const& src: m_fileReader.sourceCodes())
		if (src.first == g_stdinFileName)
			sout() << src.second << endl;
		else
		{
			ofstream outFile(src.first);
			outFile << src.second;
			if (!outFile)
			{
				serr() << "Could not write to file " << src.first << ". Aborting." << endl;
				return;
			}
		}
	sout() << "Linking completed." << endl;
}

string CommandLineInterface::libraryPlaceholderHint(string const& _libraryName)
{
	return "// " + evmasm::LinkerObject::libraryPlaceholder(_libraryName) + " -> " + _libraryName;
}

string CommandLineInterface::objectWithLinkRefsHex(evmasm::LinkerObject const& _obj)
{
	string out = _obj.toHex();
	if (!_obj.linkReferences.empty())
	{
		out += "\n";
		for (auto const& linkRef: _obj.linkReferences)
			out += "\n" + libraryPlaceholderHint(linkRef.second);
	}
	return out;
}

bool CommandLineInterface::assemble(
	yul::AssemblyStack::Language _language,
	yul::AssemblyStack::Machine _targetMachine,
	bool _optimize,
	optional<string> _yulOptimiserSteps
)
{
	solAssert(_optimize || !_yulOptimiserSteps.has_value(), "");

	bool successful = true;
	map<string, yul::AssemblyStack> assemblyStacks;
	for (auto const& src: m_fileReader.sourceCodes())
	{
		OptimiserSettings settings = _optimize ? OptimiserSettings::full() : OptimiserSettings::minimal();
		if (_yulOptimiserSteps.has_value())
			settings.yulOptimiserSteps = _yulOptimiserSteps.value();

		auto& stack = assemblyStacks[src.first] = yul::AssemblyStack(m_options.evmVersion, _language, settings);
		try
		{
			if (!stack.parseAndAnalyze(src.first, src.second))
				successful = false;
			else
				stack.optimize();
		}
		catch (Exception const& _exception)
		{
			serr() << "Exception in assembler: " << boost::diagnostic_information(_exception) << endl;
			return false;
		}
		catch (std::exception const& _e)
		{
			serr() <<
				"Unknown exception during compilation" <<
				(_e.what() ? ": " + string(_e.what()) : ".") <<
				endl;
			return false;
		}
		catch (...)
		{
			serr() << "Unknown exception in assembler." << endl;
			return false;
		}
	}

	for (auto const& sourceAndStack: assemblyStacks)
	{
		auto const& stack = sourceAndStack.second;
		SourceReferenceFormatter formatter(serr(false), coloredOutput(m_options), m_options.withErrorIds);

		for (auto const& error: stack.errors())
		{
			g_hasOutput = true;
			formatter.printErrorInformation(*error);
		}
		if (!Error::containsOnlyWarnings(stack.errors()))
			successful = false;
	}

	if (!successful)
		return false;

	for (auto const& src: m_fileReader.sourceCodes())
	{
		string machine =
			_targetMachine == yul::AssemblyStack::Machine::EVM ? "EVM" :
			"Ewasm";
		sout() << endl << "======= " << src.first << " (" << machine << ") =======" << endl;

		yul::AssemblyStack& stack = assemblyStacks[src.first];

		sout() << endl << "Pretty printed source:" << endl;
		sout() << stack.print() << endl;

		if (_language != yul::AssemblyStack::Language::Ewasm && _targetMachine == yul::AssemblyStack::Machine::Ewasm)
		{
			try
			{
				stack.translate(yul::AssemblyStack::Language::Ewasm);
				stack.optimize();
			}
			catch (Exception const& _exception)
			{
				serr() << "Exception in assembler: " << boost::diagnostic_information(_exception) << endl;
				return false;
			}
			catch (std::exception const& _e)
			{
				serr() <<
					"Unknown exception during compilation" <<
					(_e.what() ? ": " + string(_e.what()) : ".") <<
					endl;
				return false;
			}
			catch (...)
			{
				serr() << "Unknown exception in assembler." << endl;
				return false;
			}

			sout() << endl << "==========================" << endl;
			sout() << endl << "Translated source:" << endl;
			sout() << stack.print() << endl;
		}

		yul::MachineAssemblyObject object;
		try
		{
			object = stack.assemble(_targetMachine);
			object.bytecode->link(m_options.libraries);
		}
		catch (Exception const& _exception)
		{
			serr() << "Exception while assembling: " << boost::diagnostic_information(_exception) << endl;
			return false;
		}
		catch (std::exception const& _e)
		{
			serr() << "Unknown exception during compilation" << (
				_e.what() ? ": " + string(_e.what()) : "."
			) << endl;
			return false;
		}
		catch (...)
		{
			serr() << "Unknown exception while assembling." << endl;
			return false;
		}

		sout() << endl << "Binary representation:" << endl;
		if (object.bytecode)
			sout() << object.bytecode->toHex() << endl;
		else
			serr() << "No binary representation found." << endl;

		sout() << endl << "Text representation:" << endl;
		if (!object.assembly.empty())
			sout() << object.assembly << endl;
		else
			serr() << "No text representation found." << endl;
	}

	return true;
}

void CommandLineInterface::outputCompilationResults()
{
	handleCombinedJSON();

	// do we need AST output?
	handleAst();

	if (
		!m_compiler->compilationSuccessful() &&
		m_options.stopAfter == CompilerStack::State::CompilationSuccessful
	)
	{
		serr() << endl << "Compilation halted after AST generation due to errors." << endl;
		return;
	}

	vector<string> contracts = m_compiler->contractNames();
	for (string const& contract: contracts)
	{
		if (needsHumanTargetedStdout(m_options))
			sout() << endl << "======= " << contract << " =======" << endl;

		// do we need EVM assembly?
		if (m_options.selectedOutputs.asm_ || m_options.selectedOutputs.asmJson)
		{
			string ret;
			if (m_options.selectedOutputs.asmJson)
				ret = jsonPrettyPrint(removeNullMembers(m_compiler->assemblyJSON(contract)));
			else
				ret = m_compiler->assemblyString(contract, m_fileReader.sourceCodes());

			if (!m_options.outputDir.empty())
			{
				createFile(m_compiler->filesystemFriendlyName(contract) + (m_options.selectedOutputs.asmJson ? "_evm.json" : ".evm"), ret);
			}
			else
			{
				sout() << "EVM assembly:" << endl << ret << endl;
			}
		}

		if (m_options.estimateGas)
			handleGasEstimation(contract);

		handleBytecode(contract);
		handleIR(contract);
		handleIROptimized(contract);
		handleEwasm(contract);
		handleSignatureHashes(contract);
		handleMetadata(contract);
		handleABI(contract);
		handleStorageLayout(contract);
		handleNatspec(true, contract);
		handleNatspec(false, contract);
	} // end of contracts iteration

	if (!g_hasOutput)
	{
		if (!m_options.outputDir.empty())
			sout() << "Compiler run successful. Artifact(s) can be found in directory " << m_options.outputDir << "." << endl;
		else
			serr() << "Compiler run successful, no output requested." << endl;
	}
}

}
