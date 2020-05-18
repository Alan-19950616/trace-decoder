/*
 * Copyright 2019 Sifive, Inc.
 *
 * main.cpp
 */

/*
   This file is part of dqr, the SiFive Inc. Risc-V Nexus 2001 trace decoder.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cassert>

#include "dqr.hpp"

using namespace std;

static void usage(char *name)
{
	printf("Usage: dqr -t tracefile -e elffile | -n basename) [-start mn] [-stop mn] [-src] [-nosrc]\n");
	printf("           [-file] [-nofile] [-dasm] [-nodasm] [-trace] [-notrace] [--strip=path] [-v] [-h]\n");
	printf("           [-32] [-64] [-32+] [-addrsep] [-noaddrsep]\n");
	printf("\n");
	printf("-t tracefile: Specify the name of the Nexus trace message file. Must contain the file extension (such as .rtd).\n");
	printf("-e elffile:   Specify the name of the executable elf file. Must contain the file extension (such as .elf).\n");
	printf("-n basename:  Specify the base name of the Nexus trace message file and the executable elf file. No extension\n");
	printf("              should be given. The extensions .rtd and .elf will be added to basename.\n");
	printf("-start nm:    Select the Nexus trace message number to begin DQing at. The first message is 1. If -stop is\n");
	printf("              not specified, continues to last trace message.\n");
	printf("-stop nm:     Select the last Nexus trace message number to end DQing at. If -start is not specified, starts\n");
	printf("              at trace message 1.\n");
	printf("-src:         Enable display of source lines in output if available (on by default).\n");
	printf("-nosrc:       Disable display of source lines in output.\n");
	printf("-file:        Display source file information in output (on by default).\n");
	printf("-nofile:      Do not display source file information.\n");
	printf("-dasm:        Display disassembled code in output (on by default).\n");
	printf("-nodasm:      Do not display disassembled code in output.\n");
	printf("-func:        Display function name with srouce information (off by default).\n");
	printf("-nofunc:      Do not display function information with source information.\n");
	printf("-trace:       Display trace information in output (off by default).\n");
	printf("-notrace:     Do not display trace information in output.\n");
	printf("--strip=path: Strip of the specified path when displaying source file name/path. Strips off all that matches.\n");
	printf("              Path may be enclosed in quotes if it contains spaces.\n");
	printf("-itcprint:    Display ITC 0 data as a null terminated string. Data from consecutive ITC 0's will be concatenated\n");
	printf("              and displayed as a string until a terminating \\0 is found\n");
	printf("-itcprint=n:  Disaply ITC channel n data as a null terminated string. Data for consectutie ITC channel n's will be\n");
	printf("              concatinated and display as a string until a terminating \\n or \\0 is found\n");
	printf("-noitcprint:  Display ITC 0 data as a normal ITC message; address, data pair\n");
	printf("-addrsize=n:  Display address as n bits (32 <= n <= 64). Values larger than n bits will print, but take more space and\n");
	printf("              cause the address field to be jagged. Overrides value address size read from elf file.\n");
	printf("-addrsize=n+: Display address as n bits (32 <= n <= 64) unless a larger address size is seen, in which case the address\n");
	printf("              size is increased to accommodate the larger value. When the address size is increased, it stays increased\n");
	printf("              (sticky) and will be again increased if a new larger value is encountered. Overrides the address size\n");
	printf("              read from the elf file.\n");
	printf("-32:          Display addresses as 32 bits. Values lager than 32 bits will print, but take more space and cause\n");
    printf("              the address field to be jagged. Selected by default if elf file indicates 32 bit address size.\n");
	printf("              Specifying -32 overrides address size read from elf file\n");
	printf("-32+          Display addresses as 32 bits until larger addresses are displayed and then adjust up to a larger\n");
	printf("              enough size to display the entire address. When addresses are adjusted up, they do not later adjust\n");
	printf("              back down, but stay at the new size unless they need to adjust up again. This is the default setting\n");
	printf("              if the elf file specifies > 32 bit address size (such as 64). Specifying -32+ overrides the value\n");
	printf("              read from the elf file\n");
	printf("-64:          Display addresses as 64 bits. Overrides value read from elf file\n");
	printf("-addrsep:     For addresses greater than 32 bits, display the upper bits separated from the lower 32 bits by a '-'\n");
	printf("-noaddrsep:   Do not add a separator for addresses greater than 32 bit between the upper bits and the lower 32 bits\n");
	printf("              (default).\n");
	printf("-srcbits=n:   The size in bits of the src field in the trace messages. n must 0 to 8. Setting srcbits to 0 disables\n");
	printf("              multi-core. n > 0 enables multi-core. If the -srcbits=n switch is not used, srcbits is 0 by default.\n");
	printf("-analytics:   Compute and display detail level 1 trace analytics.\n");
	printf("-analytics=n: Specify the detail level for trace analytics dispaly. N sets the level to either 0 (no analytics display)\n");
	printf("              1 (sort system totals), or 2 (display analytics by core).\n");
	printf("-noanaylitics: Do not compute and display trace analytics (default). Same as -analytics=0.\n");
	printf("-freq nn      Specify the frequency in Hz for the timestamp tics clock. If specified, time instead\n");
	printf("              of tics will be displayed.\n");
	printf("-callreturn   Annotate calls, returns, and exceptions\n");
	printf("-v:           Display the version number of the DQer and exit.\n");
	printf("-h:           Display this usage information.\n");
}

static const char *stripPath(const char *prefix,const char *srcpath)
{
	if (prefix == nullptr) {
		return srcpath;
	}

	if (srcpath == nullptr) {
		return nullptr;
	}

	const char *s = srcpath;

	for (;;) {
		if (*prefix == 0) {
			return s;
		}

		if (tolower(*prefix) == tolower(*s)) {
			prefix += 1;
			s += 1;
		}
		else if (*prefix == '/') {
			if (*s != '\\') {
				return srcpath;
			}
			prefix += 1;
			s += 1;
		}
		else if (*prefix == '\\') {
			if (*s != '/') {
				return srcpath;
			}
			prefix += 1;
			s += 1;
		}
		else {
			return srcpath;
		}
	}

	return nullptr;
}

int main(int argc, char *argv[])
{
	bool binary_flag = true;
	char *tf_name = nullptr;
	char *base_name = nullptr;
	char *ef_name = nullptr;
	char buff[128];
	int buff_index = 0;
	bool usage_flag = false;
	bool version_flag = false;
	int start_msg_num = 0;
	int stop_msg_num = 0;
	bool src_flag = true;
	bool file_flag = true;
	bool dasm_flag = true;
	bool trace_flag = false;
	bool func_flag = false;
	uint32_t freq = 0;
	char *strip_flag = nullptr;
	int  numAddrBits = 0;
	uint32_t addrDispFlags = 0;
	int srcbits = 0;
	int analytics_detail = 0;
	bool itcprint_flag = false;
	int itcprint_channel = 0;
	bool showCallsReturns = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp("-t",argv[i]) == 0) {
			i += 1;
			if (i >= argc) {
				printf("Error: option -t requires a file name\n");
				usage(argv[0]);
				return 1;
			}

			base_name = nullptr;

			tf_name = argv[i];
		}
		else if (strcmp("-n",argv[i]) == 0) {
			i += 1;
			if (i >= argc) {
				printf("Error: option -n requires a file name\n");
				usage(argv[0]);
				return 1;
			}

			ef_name = nullptr;
			tf_name = nullptr;

			base_name = argv[i];
		}
		else if (strcmp("-e",argv[i]) == 0) {
			i += 1;
			if (i >= argc) {
				printf("Error: option -e requires a file name\n");
				usage(argv[0]);
				return 1;
			}

			base_name = nullptr;

			ef_name = argv[i];
		}
		else if (strcmp("-binary",argv[i]) == 0) {
			binary_flag = true;
		}
		else if (strcmp("-text",argv[i]) == 0) {
			binary_flag = false;
		}
		else if (strcmp("-start",argv[i]) == 0) {
			i += 1;
			if (i >= argc) {
				printf("Error: option -start requires a trace message number\n");
				usage(argv[0]);
				return 1;
			}

			start_msg_num = atoi(argv[i]);
			if (start_msg_num <= 0) {
				printf("Error: starting message number must be >= 1\n");
				return 1;
			}
		}
		else if (strcmp("-stop",argv[i]) == 0) {
			i += 1;
			if (i >= argc) {
				printf("Error: option -stop requires a trace message number\n");
				usage(argv[0]);
				return 1;
			}

			stop_msg_num = atoi(argv[i]);
			if (stop_msg_num <= 0) {
				printf("Error: stopping message number must be >= 1\n");
				return 1;
			}
		}
		else if (strcmp("-src",argv[i]) == 0) {
			src_flag = true;
		}
		else if (strcmp("-nosrc",argv[i]) == 0) {
			src_flag = false;
		}
		else if (strcmp("-file",argv[i]) == 0) {
			file_flag = true;
		}
		else if (strcmp("-nofile",argv[i]) == 0) {
			file_flag = false;
		}
		else if (strcmp("-dasm",argv[i]) == 0) {
			dasm_flag = true;
		}
		else if (strcmp("-nodasm",argv[i]) == 0) {
			dasm_flag = false;
		}
		else if (strcmp("-trace",argv[i]) == 0) {
			trace_flag = true;
		}
		else if (strcmp("-notrace",argv[i]) == 0) {
			trace_flag = false;
		}
		else if (strcmp("-func",argv[i]) == 0) {
			func_flag = true;
		}
		else if (strcmp("-nofunc",argv[i]) == 0) {
			func_flag = false;
		}
		else if (strncmp("--strip=",argv[i],strlen("--strip=")) == 0) {
			strip_flag = argv[i]+strlen("--strip=");

			if (strip_flag[0] == '"') {
				strip_flag += 1;

				if (strip_flag[strlen(strip_flag)] == '"') {
					strip_flag[strlen(strip_flag)] = 0;
				}
			}
		}
		else if (strcmp("-v",argv[i]) == 0) {
			version_flag = true;
		}
		else if (strcmp("-h",argv[i]) == 0) {
			usage_flag = true;
		}
		else if (strcmp("-itcprint",argv[i]) == 0) {
			itcprint_flag = true;
			itcprint_channel = 0;
		}
		else if (strncmp("-itcprint=",argv[i],strlen("-itcprint=")) == 0) {
			int l;
			char *endptr;

			l = strtol(&argv[i][strlen("-itcprint=")], &endptr, 0);

			if (endptr[0] == 0 ) {
				itcprint_flag = true;
				itcprint_channel = l;
			}
			else {
				itcprint_flag = false;
				printf("Error: option -itcprint= requires a valid number 0 - 31\n");
				usage(argv[0]);
				return 1;
			}
		}
		else if (strcmp("-noitcprint",argv[i]) == 0) {
			itcprint_flag = false;
		}
		else if (strcmp("-32",argv[i]) == 0) {
			numAddrBits = 32;
			addrDispFlags = addrDispFlags & ~TraceDqr::ADDRDISP_WIDTHAUTO;
		}
		else if (strcmp("-64",argv[i]) == 0) {
			numAddrBits = 64;
			addrDispFlags = addrDispFlags & ~TraceDqr::ADDRDISP_WIDTHAUTO;
		}
		else if (strcmp("-32+",argv[i]) == 0) {
			numAddrBits = 32;
			addrDispFlags = addrDispFlags | TraceDqr::ADDRDISP_WIDTHAUTO;
		}
		else if (strncmp("-addrsize=",argv[i],strlen("-addrsize=")) == 0) {
			int l;
			char *endptr;

			l = strtol(&argv[i][strlen("-addrsize=")], &endptr, 10);

			if (endptr[0] == 0 ) {
				numAddrBits = l;
				addrDispFlags = addrDispFlags  & ~TraceDqr::ADDRDISP_WIDTHAUTO;
			}
			else if (endptr[0] == '+') {
				numAddrBits = l;
				addrDispFlags = addrDispFlags | TraceDqr::ADDRDISP_WIDTHAUTO;
			}
			else {
				printf("Error: option -addressize= requires a valid number <= 32, >= 64\n");
				usage(argv[0]);
				return 1;
			}

			if ((l < 32) || (l > 64)) {
				printf("Error: option -addressize= requires a valid number <= 32, >= 64\n");
				usage(argv[0]);
				return 1;
			}
		}
		else if (strcmp("-addrsep", argv[i]) == 0) {
			addrDispFlags = addrDispFlags | TraceDqr::ADDRDISP_SEP;
		}
		else if (strcmp("-noaddrsep", argv[i]) == 0) {
			addrDispFlags = addrDispFlags & ~TraceDqr::ADDRDISP_SEP;
		}
		else if (strncmp("-srcbits=",argv[i],strlen("-srcbits=")) == 0) {
			srcbits = atoi(argv[i]+strlen("-srcbits="));

			if ((srcbits < 0) || (srcbits > 8)) {
				printf("Error: option -srcbits=n, n must be a valid number of trace message src bits >= 0, <= 8\n");
				usage(argv[0]);
				return 1;
			}
		}
		else if (strcmp("-analytics",argv[i]) == 0) {
			analytics_detail = 1;
		}
		else if (strncmp("-analytics=",argv[i],strlen("-analytics=")) == 0) {
			analytics_detail = atoi(argv[i]+strlen("-analytics="));

			if (analytics_detail < 0) {
				printf("Error: option -analytics=n, n must be a valid number >= 0\n");
				usage(argv[0]);
				return 1;
			}
		}
		else if (strcmp("-noanalytics",argv[i]) == 0) {
			analytics_detail = 0;
		}
		else if (strcmp("-freq",argv[i]) == 0) {
			i += 1;
			if (i >= argc) {
				printf("Error: option -freq requires a clock frequency to be specified\n");
				usage(argv[0]);
				return 1;
			}

			freq = atoi(argv[i]);
			if (freq < 0) {
				printf("Error: clock frequency must be >= 0\n");
				return 1;
			}
		}
		else if (strcmp("-callreturn",argv[i]) == 0 ) {
			showCallsReturns = true;
		}
		else {
			printf("Unkown option '%s'\n",argv[i]);
			usage_flag = true;
		}
	}

	if (usage_flag) {
		usage(argv[0]);
		return 0;
	}

	if (version_flag) {
		printf("%s: version %s\n",argv[0],DQR_VERSION);
		return 0;
	}

	if (tf_name == nullptr) {
		if (base_name == nullptr) {
			printf("Error: must specify either a base name or a trace file name\n");
			usage(argv[0]);
			return 1;
		}

		tf_name = &buff[buff_index];
		strcpy(tf_name,base_name);
		strcat(tf_name,".rtd");
		buff_index += strlen(tf_name) + 1;
	}

	if (ef_name == nullptr) {
		if (base_name != nullptr) {
			ef_name = &buff[buff_index];
			strcpy(ef_name,base_name);
			strcat(ef_name,".elf");
			buff_index += strlen(ef_name) + 1;
		}
	}

	// might want to include some path info!

	Trace *trace = new (std::nothrow) Trace(tf_name,binary_flag,ef_name,numAddrBits,addrDispFlags,srcbits,freq);

	assert(trace != nullptr);

	if (trace->getStatus() != TraceDqr::DQERR_OK) {
		delete trace;
		trace = nullptr;

		printf("Error: new Trace(%s,%s) failed\n",tf_name,ef_name);

		return 1;
	}

	trace->setTraceRange(start_msg_num,stop_msg_num);

	if (itcprint_flag) {
		trace->setITCPrintOptions(4096,itcprint_channel);
	}

	TraceDqr::DQErr ec;

	// main loop

//	this shouldn't be next instruction. it should be next because we may not be generating instruction'
//	we may just be dumping raw traces, or we may be dumping traces with addresses or we may be doing
//	dissasembled instruction traces or we may be adding source code
//
//	flags:
//
//		raw trace messages
//		decoded trace message
//		include trace addresses
//		include disassembly
//		include source

	// we want to be able to select the level of output, from minimal (raw trace only) to full (everything)

//	do we select when we create the trace object?
//	does it always generate as much as possible and we jsut print what we want?
//
//	should not print, but should return a string or way to make a string!
//
//	should look at source code display!

	Instruction *instInfo;
	NexusMessage *msgInfo;
	Source *srcInfo;
	char dst[10000];
	int instlevel = 1;
	int msgLevel = 2;
	const char *lastSrcFile = nullptr;
	const char *lastSrcLine = nullptr;
	unsigned int lastSrcLineNum = 0;
	TraceDqr::ADDRESS lastAddress = 0;
	int lastInstSize = 0;
	bool firstPrint = true;
	uint32_t core_mask = 0;
	TraceDqr::TIMESTAMP startTime, endTime;

	do {
		ec = trace->NextInstruction(&instInfo,&msgInfo,&srcInfo);

		if (ec == TraceDqr::DQERR_OK) {
			if (srcInfo != nullptr) {
				if ((lastSrcFile != srcInfo->sourceFile) || (lastSrcLine != srcInfo->sourceLine) || (lastSrcLineNum != srcInfo->sourceLineNum)) {
					lastSrcFile = srcInfo->sourceFile;
					lastSrcLine = srcInfo->sourceLine;
					lastSrcLineNum = srcInfo->sourceLineNum;

					if (file_flag) {
						if (srcInfo->sourceFile != nullptr) {
							if (firstPrint == false) {
								printf("\n");
							}

							const char *sfp;

							sfp = stripPath(strip_flag,srcInfo->sourceFile);

							if (srcbits > 0) {
								printf("[%d] File: %s:%d\n",srcInfo->coreId,sfp,srcInfo->sourceLineNum);
							}
							else {
								printf("File: %s:%d\n",sfp,srcInfo->sourceLineNum);
							}

							firstPrint = false;
						}
					}

					if (src_flag) {
						if (srcInfo->sourceLine != nullptr) {
							if (srcbits > 0) {
								printf("[%d] Source: %s\n",srcInfo->coreId,srcInfo->sourceLine);
							}
							else {
								printf("Source: %s\n",srcInfo->sourceLine);
							}

							firstPrint = false;
						}
					}
				}
			}

			if (dasm_flag && (instInfo != nullptr)) {
//			    instInfo->addressToText(dst,instlevel);

				instInfo->addressToText(dst,sizeof dst,0);

				if (func_flag) {
					if (srcbits > 0) {
						printf("[%d] ",instInfo->coreId);
					}

					if (instInfo->address != (lastAddress + lastInstSize / 8)) {
						if (instInfo->addressLabel != nullptr) {
							printf("<%s",instInfo->addressLabel);
							if (instInfo->addressLabelOffset != 0) {
								printf("+%x",instInfo->addressLabelOffset);
							}
							printf(">\n");
						}
					}

					lastAddress = instInfo->address;
					lastInstSize = instInfo->instSize;
				}

				if (srcbits > 0) {
					printf("[%d] ", instInfo->coreId);
				}

				int n;

				n = printf("    %s:",dst);

				for (int i = n; i < 20; i++) {
					printf(" ");
				}

				instInfo->instructionToText(dst,sizeof dst,instlevel);
				printf("  %s",dst);

				if (showCallsReturns == true) {
					switch (instInfo->CRFlag) {
					case TraceDqr::isCall:
						printf(" [call]");
						break;
					case TraceDqr::isReturn:
						printf(" [return]");
						break;
					case TraceDqr::isSwap:
						printf(" [coroutine swap]");
						break;
					case TraceDqr::isException:
						printf(" [exception]");
						break;
					case TraceDqr::isExceptionReturn:
						printf(" [exception return]");
						break;
					case TraceDqr::isOther:
					default:
						break;
					}
				}

				printf("\n");

				firstPrint = false;
			}

			if ((trace_flag || itcprint_flag) && (msgInfo != nullptr)) {
				// got the goods! Get to it!

				core_mask |= 1 << msgInfo->coreId;

				msgInfo->messageToText(dst,sizeof dst,msgLevel);

				if (trace_flag) {
					if (firstPrint == false) {
						printf("\n");
					}

					if (srcbits > 0) {
						printf("[%d] ",msgInfo->coreId);
					}

					printf("Trace: %s",dst);

					printf("\n");

					firstPrint = false;
				}

				if (itcprint_flag) {
					std::string s;
					bool haveStr;

					s = trace->getITCPrintStr(msgInfo->coreId,haveStr,startTime,endTime);
					while (haveStr != false) {
						if (firstPrint == false) {
							printf("\n");
						}

						if (srcbits > 0) {
							printf("[%d] ",msgInfo->coreId);
						}

						std::cout << "ITC Print: ";

						if ((startTime != 0) || (endTime != 0)) {
							std::cout << "Msg Tics: <" << startTime << "-" << endTime << "> ";
						}

						std::cout << s;

						firstPrint = false;

						s = trace->getITCPrintStr(msgInfo->coreId,haveStr,startTime,endTime);
					}
				}
			}
		}
	} while (ec == TraceDqr::DQERR_OK);

	if (itcprint_flag) {
		std::string s = "";
		bool haveStr;

		for (int core = 0; core_mask != 0; core++) {
			if (core_mask & 1) {
				s = trace->flushITCPrintStr(core,haveStr,startTime,endTime);
				while (haveStr != false) {
					if (firstPrint == false) {
						printf("\n");
					}

					if (srcbits > 0) {
						printf("[%d] ",core);
					}

					std::cout << "ITC Print: ";

					if ((startTime != 0) || (endTime != 0)) {
						std::cout << "Msg Tics: <" << startTime << "-" << endTime << "> ";
					}

					std::cout << s;

					firstPrint = false;

					s = trace->flushITCPrintStr(core,haveStr,startTime,endTime);
				}
			}
			core_mask >>= 1;
		}
	}

	if (analytics_detail > 0) {
		trace->analyticsToText(dst,sizeof dst,analytics_detail);
		if (firstPrint == false) {
			printf("\n");
		}
		firstPrint = false;
		printf("%s",dst);
	}

	delete trace;

	return 0;
}
