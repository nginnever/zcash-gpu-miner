/* MIT License
 *
 * Copyright (c) 2016 str4d
 * Copyright (c) 2016 Omar Alvarez <omar.alvarez@udc.es>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "arith_uint256.h"
#include "chainparams.h"
#include "crypto/equihash.h"
#include "libstratum/StratumClient.h"
#include "primitives/block.h"
#include "serialize.h"
#include "streams.h"
#include "util.h"
#include "utiltime.h"
#include "version.h"

#include "libzogminer/gpusolver.h"
#include "libzogminer/gpuconfig.h"
#include "libzogminer/cl_zogminer.h"

#include "sodium.h"

#include <csignal>
#include <iostream>

static uint64_t rdtsc(void) {
#ifdef _MSC_VER
    return __rdtsc();
#else
#if defined(__amd64__) || defined(__x86_64__)
    uint64_t rax, rdx;
    __asm__ __volatile__("rdtsc" : "=a"(rax), "=d"(rdx) : : );
    return (rdx << 32) | rax;
#elif defined(__i386__) || defined(__i386) || defined(__X86__)
    uint64_t rax;
    __asm__ __volatile__("rdtsc" : "=A"(rax) : : );
    return rax;
#else
#error "Not implemented!"
#endif
#endif
}

std::string HelpMessageMiner()
{
    string strUsage;
    strUsage += HelpMessageGroup(_("Options:"));
    strUsage += HelpMessageOpt("-?", _("This help message"));

    strUsage += HelpMessageGroup(_("Mining pool options:"));
    strUsage += HelpMessageOpt("-stratum=<url>", _("Mine on the Stratum server at <url>"));
    strUsage += HelpMessageOpt("-user=<user>",
                               strprintf(_("Username for Stratum server (default: %u)"), "x"));
    strUsage += HelpMessageOpt("-password=<pw>",
                               strprintf(_("Password for Stratum server (default: %u)"), "x"));

    strUsage += HelpMessageGroup(_("Debugging/Testing options:"));
    string debugCategories = "cycles, pow, stratum"; // Don't translate these
    strUsage += HelpMessageOpt("-debug=<category>", strprintf(_("Output debugging information (default: %u, supplying <category> is optional)"), 0) + ". " +
        _("If <category> is not supplied, output all debugging information.") + " " + _("<category> can be:") + " " + debugCategories + ".");
    strUsage += HelpMessageOpt("-printtoconsole", _("Send trace/debug info to console instead of debug.log file"));
    strUsage += HelpMessageOpt("-regtest", _("Enter regression test mode, which uses a special chain in which blocks can be "
                                             "solved instantly. This is intended for regression testing tools and app development."));
    strUsage += HelpMessageOpt("-testnet", _("Use the test network"));
	strUsage += HelpMessageOpt("-G", _("GPU mine"));
	strUsage += HelpMessageOpt("-S=<deviceid>", _("Select GPU device (default: 0)"));
	strUsage += HelpMessageOpt("-listdevices", _("List available OpenCL devices"));

    return strUsage;
}

std::string LicenseInfo()
{
    return FormatParagraph(_("Copyright (C) 2009-2015 The Bitcoin Core Developers")) + "\n" +
           FormatParagraph(strprintf(_("Copyright (C) 2015-%i The Zcash Developers"), COPYRIGHT_YEAR)) + "\n" +
           FormatParagraph(strprintf(_("Copyright (C) %i Jack Grigg <jack@z.cash>"), COPYRIGHT_YEAR)) + "\n" +
           "\n" +
           FormatParagraph(_("This is experimental software.")) + "\n" +
           "\n" +
           FormatParagraph(_("Distributed under the MIT software license, see the accompanying file COPYING or <http://www.opensource.org/licenses/mit-license.php>.")) + "\n" +
           "\n";
}

void test_mine(int n, int k, uint32_t d, GPUConfig conf)
{
    CBlock pblock;
    pblock.nBits = d;
    arith_uint256 hashTarget = arith_uint256().SetCompact(d);
	GPUSolver * solver;
	if(conf.useGPU)
    	solver = new GPUSolver(conf.selGPU);

	uint64_t nn= 0;
	//TODO Free
	uint8_t * header = (uint8_t *) calloc(ZCASH_BLOCK_HEADER_LEN, sizeof(uint8_t));
	

    while (true) {
        // Hash state
        crypto_generichash_blake2b_state state;
        EhInitialiseState(n, k, state);

        // I = the block header minus nonce and solution.
        CEquihashInput I{pblock};
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << I;
		
		//std::cout << "ss size: "<< ss.size() << std::endl;
		memcpy(header, &ss[0], ss.size());
        // H(I||...
        crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());

        // Find valid nonce
        int64_t nStart = GetTime();
        uint64_t start_cycles = rdtsc();
        while (true) {
            // H(I||V||...
            crypto_generichash_blake2b_state curr_state;
            curr_state = state;
            //crypto_generichash_blake2b_update(&curr_state,
            //                                  pblock.nNonce.begin(),
            //                                  pblock.nNonce.size());

			//std::cout << "nonce size: "<< sizeof(uint256) << " " << sizeof(uint64_t )  << std::endl;

            // (x_1, x_2, ...) = A(I, V, n, k)
            LogPrint("pow", "Running Equihash solver with nNonce = %s\n",
                     pblock.nNonce.ToString());
			

            std::function<bool(std::vector<unsigned char>)> validBlock =
                    [&pblock, &hashTarget, &nStart, &start_cycles]
                    (std::vector<unsigned char> soln) {
                // Write the solution to the hash and compute the result.
                pblock.nSolution = soln;

                if (UintToArith256(pblock.GetHash()) > hashTarget) {
                    return false;
                }

                // Found a solution
                uint64_t stop_cycles = rdtsc();
                LogPrintf("ZcashMiner:\nproof-of-work found\nprevHash: %s\n    hash: %s\n  target: %s\nduration: %s\n",
                          pblock.hashPrevBlock.GetHex(),
                          pblock.GetHash().GetHex(),
                          hashTarget.GetHex(),
                          (GetTime() - nStart));
                LogPrint("cycles", "  cycles: %2.2f Mcycles\n",
                         (double)(stop_cycles - start_cycles) / (1UL << 20));

                return true;
            };
            std::function<bool(GPUSolverCancelCheck)> cancelledGPU =
                    [](GPUSolverCancelCheck pos) {
                return false;
            };
			std::function<bool(EhSolverCancelCheck)> cancelled =
                    [](EhSolverCancelCheck pos) {
                return false;
            };
            try {
                uint64_t solve_start = rdtsc();
				bool foundBlock;
				if(!conf.useGPU)
                	foundBlock = EhOptimisedSolve(n, k, curr_state, validBlock, cancelled);
				else
					foundBlock = solver->run(n, k, header, ZCASH_BLOCK_HEADER_LEN - ZCASH_NONCE_LEN, nn++, validBlock, cancelledGPU, curr_state);
                    uint64_t solve_end = rdtsc();
                    LogPrint("cycles", "Solver took %2.2f Mcycles\n\n",
                    (double)(solve_end - solve_start) / (1UL << 20));
                    // If we find a valid block, we rebuild
                if (foundBlock) {
                    break;
                }
            } catch (GPUSolverCancelledException&) {
                LogPrint("pow", "Equihash solver cancelled\n");
            }

            if ((UintToArith256(pblock.nNonce) & 0xffff) == 0xffff) {
                break;
            }
            pblock.nNonce = ArithToUint256(UintToArith256(pblock.nNonce) + 1);
        }

        pblock.hashPrevBlock = pblock.GetHash();
    }

	if(conf.useGPU)
		delete solver;

}

static ZcashStratumClient* scSig;
extern "C" void stratum_sigint_handler(int signum) {if (scSig) scSig->disconnect();}

int main(int argc, char* argv[])
{
    ParseParameters(argc, argv);

    // Process help and version first
    if (mapArgs.count("-?") || mapArgs.count("-h") ||
        mapArgs.count("-help") || mapArgs.count("-version")) {
        std::string strUsage = _("Zcash Miner") + " " +
                               _("version") + " " + FormatFullVersion() + "\n";

        if (mapArgs.count("-version")) {
            strUsage += LicenseInfo();
        } else {
            strUsage += "\n" + _("Usage:") + "\n" +
                  "  zcash-miner [options]                     " + _("Start Zcash Miner") + "\n";

            strUsage += "\n" + HelpMessageMiner();
        }

        std::cout << strUsage;
        return 1;
    }
	
	if(GetBoolArg("-listdevices", false)) {
		//Generic Things
		std::cout << "Number of Platforms:" << cl_zogminer::getNumPlatforms << "\n";
		cl_zogminer::listDevices();
		return 0;
	}

	GPUConfig conf;

	conf.useGPU = GetBoolArg("-G", false) || GetBoolArg("-GPU", false);
	conf.selGPU = GetArg("-S", 0);
	conf.allGPU = GetBoolArg("-allgpu", false);
	conf.forceGenProcLimit = GetBoolArg("-forcenolimit", false);
	int nThreads = GetArg("-genproclimit", 1);
	//std::cout << GPU << " " << selGPU << std::endl;

    // Zcash debugging
    fDebug = !mapMultiArgs["-debug"].empty();
    fPrintToConsole = GetBoolArg("-printtoconsole", false);

    // Check for -testnet or -regtest parameter
    // (Params() calls are only valid after this clause)
    if (!SelectParamsFromCommandLine()) {
        std::cerr << "Error: Invalid combination of -regtest and -testnet." << std::endl;
        return 1;
    }

    // Initialise libsodium
    if (init_and_check_sodium() == -1) {
        return 1;
    }

    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("Zcash Miner version %s (%s)\n", FormatFullVersion(), CLIENT_DATE);		

    // Start the mining operation
	

    std::string stratum = GetArg("-stratum", "");
    if (!stratum.empty() || GetBoolArg("-stratum", false)) {
        if (stratum.compare(0, 14, "stratum+tcp://") != 0) {
            std::cerr << "Error: -stratum must be a stratum+tcp:// URL." << std::endl;
            return false;
        }

        std::string host;
        std::string port;
        std::string stratumServer = stratum.substr(14);
        size_t delim = stratumServer.find(':');
        if (delim != std::string::npos) {
            host = stratumServer.substr(0, delim);
            port = stratumServer.substr(delim+1);
        }
        if (host.empty() || port.empty()) {
            std::cerr << "Error: -stratum must contain a host and port." << std::endl;
            return false;
        }

		ZcashMiner miner(GetArg("-genproclimit", 1), conf);
        ZcashStratumClient sc {
            &miner, host, port,
            GetArg("-user", "x"),
            GetArg("-password", "x"),
            0, 0
        };

        miner.onSolutionFound([&](const EquihashSolution& solution) {
            return sc.submit(&solution);
        });

        scSig = &sc;
        signal(SIGINT, stratum_sigint_handler);	

        while(sc.isRunning()) {
            MilliSleep(1000);
        }
    } else {
        std::cout << "Running the test miner" << std::endl;
        test_mine(
            Params().EquihashN(),
            Params().EquihashK(),
            0x200f0f0f, conf);
    }

    return 0;
}
