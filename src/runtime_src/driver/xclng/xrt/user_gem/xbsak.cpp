/**
 * Copyright (C) 2016-2018 Xilinx, Inc
 * Author: Hem C Neema, Ryan Radjabi
 * Simple command line utility to inetract with SDX PCIe devices
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "xbsak.h"
#include "shim.h"
#include <linux/limits.h>

int xcldev::xclXbsak(int argc, char *argv[])
{
	std::cout << std::endl;
	std::cout << "***** THIS IS AN EXPERIMENTAL VERSION OF XBSAK IMPLEMENTING xbsak status --sam --bar AND POWER PROFILING *****" << std::endl;
	std::cout << std::endl;

	unsigned sampleFreq = 1;
	unsigned int baseBarAddr = 0x0;
	unsigned int readBarSize = 4;
    unsigned index = 0xffffffff;
    unsigned regionIndex = 0xffffffff;
    unsigned computeIndex = 0xffffffff;
    unsigned short targetFreq[2] = {0, 0};
    unsigned fanSpeed = 0;
    unsigned long long startAddr = 0;
    unsigned int pattern_byte = 'J';//Rather than zero; writing char 'J' by default
    size_t sizeInBytes = 0;
    std::string outMemReadFile = "memread.out";
    std::string powerTraceFile = "power_trace.csv";
    std::string barFile = "bar_counters.out";
    std::string flashType = ""; // unset and empty by default
    std::string mcsFile1, mcsFile2;
    std::string xclbin;
    size_t blockSize = 0x200000;
    bool hot = false;
    bool outputBarFile = false;
    int c;
    dd::ddArgs_t ddArgs;

    const char* exe = argv[ 0 ];
    if (argc == 1) {
        xcldev::printHelp(exe);
        return 1;
    }

    /*
     * Call xbflash if first argument is "flash". This calls
     * xbflash and never returns. All arguments will be passed
     * down to xbflash.
     */
    if( std::string( argv[ 1 ] ).compare( "flash" ) == 0 ) {
        // get self path, launch xbflash from self path
        char buf[ PATH_MAX ] = {0};
        auto len = readlink( "/proc/self/exe", buf, PATH_MAX );
        if( len == -1 ) {
            perror( "readlink:" );
            return errno;
        }
        buf[ len - 1 ] = 0; // null terminate after successful read

        // remove exe name from this to get the parent path
        size_t found = std::string( buf ).find_last_of( "/\\" ); // finds the last backslash char
        std::string path = std::string( buf ).substr( 0, found );
        return execv( std::string( path + "/xbflash" ).c_str(), argv );
    } /* end of call to xbflash */

    argv++;
    const auto v = xcldev::commandTable.find(*argv);
    if (v == xcldev::commandTable.end()) {
        std::cout << "ERROR: Unknown comand \'" << *argv << "\'\n";
        xcldev::printHelp(exe);
        return 1;
    }

    const xcldev::command cmd = v->second;
    std::string cmdname = v->first;
    xcldev::subcommand subcmd = xcldev::MEM_READ;
    unsigned int ipmask = static_cast<unsigned int>(xcldev::STATUS_NONE_MASK);
    argc--;

    if (cmd == xcldev::HELP) {
        xcldev::printHelp(exe);
        return 1;
    }

    argv[0] = const_cast<char *>(exe);
    static struct option long_options[] = {
	{"read", no_argument, 0, xcldev::MEM_READ},
	{"write", no_argument, 0, xcldev::MEM_WRITE},
	{"spm", no_argument, 0, xcldev::STATUS_SPM},
	{"lapc", no_argument, 0, xcldev::STATUS_LAPC},
	{"tracefunnel", no_argument, 0, xcldev::STATUS_UNSUPPORTED},
	{"monitorfifolite", no_argument, 0, xcldev::STATUS_UNSUPPORTED},
	{"monitorfifofull", no_argument, 0, xcldev::STATUS_UNSUPPORTED},
	{"accelmonitor", no_argument, 0, xcldev::STATUS_UNSUPPORTED},
	{"sam", no_argument, 0, xcldev::STATUS_SAM},
	{"bar", no_argument, 0, xcldev::STATUS_BAR},
	{"once", no_argument, 0, xcldev::POWER_ONCE},
	{"trace", no_argument, 0, xcldev::POWER_TRACE}
    };
    int long_index;
    const char* short_options = "a:d:e:i:r:p:f:g:m:n:c:s:b:ho:"; //don't add numbers
    while ((c = getopt_long(argc, argv, short_options, long_options, &long_index)) != -1)
    {
        if (cmd == xcldev::LIST) {
            std::cout << "ERROR: 'list' command does not accept any options\n";
            return -1;
        }
        switch (c)
        {
        //Deal with long options. Long options return the value set in option::val
        case xcldev::MEM_READ : {
            //--read
            if (cmd != xcldev::MEM) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            subcmd = xcldev::MEM_READ;
            break;
        }
        case xcldev::MEM_WRITE : {
            //--write
            if (cmd != xcldev::MEM) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            subcmd = xcldev::MEM_WRITE;
            break;
        }
        case xcldev::STATUS_LAPC : {
            //--lapc
            if (cmd != xcldev::STATUS) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            ipmask |= static_cast<unsigned int>(xcldev::STATUS_LAPC_MASK);
            break;
        }
        case xcldev::STATUS_SPM : {
            //--spm
            if (cmd != xcldev::STATUS) {
                std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
                return -1;
            }
            ipmask |= static_cast<unsigned int>(xcldev::STATUS_SPM_MASK);
            break;
        }
        case xcldev::STATUS_SAM : {
        	if (cmd != xcldev::STATUS) {
				std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
				return -1;
			}
        	ipmask |= static_cast<unsigned int>(xcldev::STATUS_SAM_MASK);
        	break;
        }
        case xcldev::STATUS_BAR : {
        	if (cmd != xcldev::STATUS) {
        		std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
        		return -1;
        	}
        	ipmask |= static_cast<unsigned int>(xcldev::STATUS_BAR_MASK);
        	break;
        }
        case xcldev::STATUS_UNSUPPORTED : {
            //Don't give ERROR for as yet unsupported IPs
            std::cout << "INFO: No Status information available for IP: " << long_options[long_index].name << "\n";
            return 0;
        }
        case xcldev::POWER_ONCE : {
			if (cmd != xcldev::POWER) {
				std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
				return -1;
			}
			ipmask |= static_cast<unsigned int>(xcldev::POWER_ONCE_MASK);
			break;
		}
		case xcldev::POWER_TRACE : {
			if (cmd != xcldev::POWER) {
				std::cout << "ERROR: Option '" << long_options[long_index].name << "' cannot be used with command " << cmdname << "\n";
				return -1;
			}
			ipmask |= static_cast<unsigned int>(xcldev::POWER_TRACE_MASK);
			break;
		}
            //short options are dealt here
        case 'a':{
            if (cmd != xcldev::MEM && cmd != xcldev::STATUS) {
                std::cout << "ERROR: '-a' not applicable for this command\n";
                return -1;
            }
            size_t idx = 0;
            try {
            	unsigned long long tmpAddr = std::stoll(optarg, &idx, 0);
                startAddr = tmpAddr;
                baseBarAddr = (unsigned int)tmpAddr;
            }
            catch (const std::exception& ex) {
                //out of range, invalid argument ex
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            if (idx < strlen(optarg)) {
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            break;
        }
        case 'o': {
            if (cmd == xcldev::FLASH) {
                flashType = optarg;
                break;
            } else if (cmd == xcldev::POWER) {
            	powerTraceFile = optarg;
            	break;
            } else if (cmd == xcldev::STATUS) {
            	barFile = optarg;
            	outputBarFile = true;
            	break;
            } else if (cmd != xcldev::MEM || subcmd != xcldev::MEM_READ) {
                std::cout << "ERROR: '-o' not applicable for this command\n";
                return -1;
            }
            outMemReadFile = optarg;
            break;
        }
        case 'e': {
            if (cmd != xcldev::MEM || subcmd != xcldev::MEM_WRITE) {
                std::cout << "ERROR: '-e' not applicable for this command\n";
                return -1;
            }
            size_t idx = 0;
            try {
                pattern_byte = std::stoi(optarg, &idx, 0);
            }
            catch (const std::exception& ex) {
                //out of range, invalid argument ex
                std::cout << "ERROR: Value supplied to -" << (char)c << " option must be a value between 0 and 255\n";
                return -1;
            }
            if (pattern_byte > 0xff || idx < strlen(optarg)) {
                std::cout << "ERROR: Value supplied to -" << (char)c << " option must be a value between 0 and 255\n";
                return -1;
            }
            break;
        }
        case 'i': {
            if (cmd != xcldev::MEM && cmd != xcldev::STATUS) {
                std::cout << "ERROR: '-i' not applicable for this command\n";
                return -1;
            }
            size_t idx = 0;
            try {
            	size_t tmpSizeInBytes = std::stoll(optarg, &idx, 0);
                sizeInBytes = tmpSizeInBytes;
                readBarSize = (unsigned int)tmpSizeInBytes;
            }
            catch (const std::exception& ex) {
                //out of range, invalid argument ex
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            if (idx < strlen(optarg)) {
                std::cout << "ERROR: Value supplied to -" << (char)c << " option is invalid\n";
                return -1;
            }
            break;
        }
        case 'd':
            index = std::atoi(optarg);
            if (cmd == xcldev::DD) {
                ddArgs = dd::parse_dd_options( argc, argv );
            }
            break;
        case 'r':
            if ((cmd == xcldev::FLASH) || (cmd == xcldev::BOOT) || (cmd == xcldev::DMATEST) ||(cmd == xcldev::STATUS)) {
                std::cout << "ERROR: '-r' not applicable for this command\n";
                return -1;
            }
            regionIndex = std::atoi(optarg);
            break;
        case 'p':
            if (cmd != xcldev::PROGRAM) {
                std::cout << "ERROR: '-p' only allowed with 'program' command\n";
                return -1;
            }
            xclbin = optarg;
            break;
        case 'f':
            if (cmd != xcldev::CLOCK && cmd != xcldev::POWER) {
                std::cout << "ERROR: '-f' only allowed with 'clock' or 'power --trace' command\n";
                return -1;
            }
            if (cmd == xcldev::CLOCK) {
            	targetFreq[0] = std::atoi(optarg);
            }
            if (cmd == xcldev::POWER) {
            	sampleFreq = std::atoi(optarg);
            }
            break;
        case 'g':
            if (cmd != xcldev::CLOCK) {
                std::cout << "ERROR: '-g' only allowed with 'clock' command\n";
                return -1;
            }
            targetFreq[1] = std::atoi(optarg);
            break;
        case 'm':
            if (cmd != xcldev::FLASH) {
                std::cout << "ERROR: '-m' only allowed with 'flash' command\n";
                return -1;
            }
            mcsFile1 = optarg;
            break;
        case 'n':
            if (cmd != xcldev::FLASH) {
                std::cout << "ERROR: '-n' only allowed with 'flash' command\n";
                return -1;
            }
            mcsFile2 = optarg;
            break;
        case 'c':
            if (cmd != xcldev::RUN) {
                std::cout << "ERROR: '-c' only allowed with 'run' command\n";
                return -1;
            }
            computeIndex = std::atoi(optarg);
            break;
        case 's':
            if (cmd != xcldev::FAN) {
                std::cout << "ERROR: '-s' only allowed with 'fan' command\n";
                return -1;
            }
            fanSpeed = std::atoi(optarg);
            break;
        case 'b':
        {
            if (cmd != xcldev::DMATEST) {
                std::cout << "ERROR: '-b' only allowed with 'dmatest' command\n";
                return -1;
            }
            std::string tmp(optarg);
            if ((tmp[0] == '0') && (std::tolower(tmp[1]) == 'x')) {
                blockSize = std::stoll(tmp, 0, 16);
            }
            else {
                blockSize = std::stoll(tmp, 0, 10);
            }

            if (blockSize & (blockSize - 1)) {
                std::cout << "ERROR: block size should be power of 2\n";
                return -1;
            }

            if (blockSize > 0x100000) {
                std::cout << "ERROR: block size cannot be greater than 0x100000 MB\n";
                return -1;
            }
            blockSize *= 1024; // convert kilo bytes to bytes
            break;
        }
        case 'h':
        {
            if (cmd != xcldev::RESET) {
                std::cout << "ERROR: '-h' only allowed with 'reset' command\n";
                return -1;
            }
            hot = true;
            break;
        }
        default:
            xcldev::printHelp(exe);
            return 1;
        }
    }

    if (optind != argc) {
        std::cout << "ERROR: Illegal command \'" << argv[optind++] << "\'\n";
        return -1;
    }

    if (index == 0xffffffff) index = 0;

    if (regionIndex == 0xffffffff) regionIndex = 0;

    switch (cmd) {
    case xcldev::BOOT:
    case xcldev::RUN:
    case xcldev::FAN:
    case xcldev::DMATEST:
    case xcldev::MEM:
    case xcldev::QUERY:
    case xcldev::SCAN:
    case xcldev::STATUS:
        break;
    case xcldev::PROGRAM:
    {
        if (xclbin.size() == 0) {
            std::cout << "ERROR: Please specify xclbin file with '-p' switch\n";
            return -1;
        }
        break;
    }
    case xcldev::CLOCK:
    {
        if (!targetFreq[0] && !targetFreq[1]) {
            std::cout << "ERROR: Please specify frequency(ies) with '-f' and or '-g' switch(es)\n";
            return -1;
        }
        break;
    }
    default:
        break;
    }

    if (cmd == xcldev::SCAN) {
        xcldev::pci_device_scanner devScanner;
        return devScanner.scan(true);
    }

    std::vector<std::unique_ptr<xcldev::device>> deviceVec;

    try {
        unsigned int count = xclProbe();
        if (count == 0) {
            std::cout << "ERROR: No devices found\n";
            return 1;
        }

        for (unsigned i = 0; i < count; i++) {
            deviceVec.emplace_back(new xcldev::device(i, nullptr));
        }
    }
    catch (const std::exception& ex) {
        std::cout << ex.what() << std::endl;
        return 1;
    }

    std::cout << "INFO: Found " << deviceVec.size() << " device(s)\n";

    if (cmd == xcldev::LIST) {
        for (unsigned i = 0; i < deviceVec.size(); i++) {
            std::cout << '[' << i << "] " << deviceVec[i]->name() << std::endl;
        }
        return 0;
    }

    if (index >= deviceVec.size()) {
        std::cout << "ERROR: Device index " << index << " out of range\n";
        return 1;
    }

    int result = 0;

    switch (cmd)
    {
    case xcldev::BOOT:
        result = deviceVec[index]->boot();
        break;
    case xcldev::CLOCK:
        result = deviceVec[index]->reclock2(regionIndex, targetFreq);
        break;
    case xcldev::FAN:
        result = deviceVec[index]->fan(fanSpeed);
        break;
    case xcldev::FLASH:
        result = deviceVec[index]->flash(mcsFile1, mcsFile2, flashType);
        break;
    case xcldev::PROGRAM:
        result = deviceVec[index]->program(xclbin, regionIndex);
        break;
    case xcldev::QUERY:
        result = deviceVec[index]->dump(std::cout);
        break;
    case xcldev::VALIDATE:
        result = deviceVec[index]->validate();
        break;
    case xcldev::RESET:
        if (hot) regionIndex = 0xffffffff;
        result = deviceVec[index]->reset(regionIndex);
        break;
    case xcldev::RUN:
        result = deviceVec[index]->run(regionIndex, computeIndex);
        break;
    case xcldev::DMATEST:
        result = deviceVec[index]->dmatest(blockSize);
        break;
    case xcldev::MEM:
        if (subcmd == xcldev::MEM_READ) {
            result = deviceVec[index]->memread(outMemReadFile, startAddr, sizeInBytes);
        }
        else if (subcmd == xcldev::MEM_WRITE) {
            result = deviceVec[index]->memwrite(startAddr, sizeInBytes, pattern_byte);
        }
        break;
    case xcldev::DD:
        result = deviceVec[index]->do_dd( ddArgs );
        break;
    case xcldev::STATUS:
        if (ipmask == xcldev::STATUS_NONE_MASK) {
            //if no ip specified then read all
            //ipmask = static_cast<unsigned int>(xcldev::STATUS_SPM_MASK);
            //if (!(getuid() && geteuid())) {
            //  ipmask |= static_cast<unsigned int>(xcldev::STATUS_LAPC_MASK);
            //}
            result = deviceVec[index]->print_debug_ip_list(0);
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_LAPC_MASK)) {
            result = deviceVec[index]->readLAPCheckers(1);
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_SPM_MASK)) {
            result = deviceVec[index]->readSPMCounters();
        }
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_SAM_MASK)) {
			result = deviceVec[index]->readSAMCounters();
		}
        if (ipmask & static_cast<unsigned int>(xcldev::STATUS_BAR_MASK)) {
			result = deviceVec[index]->readBarCounters(baseBarAddr, readBarSize, barFile, outputBarFile);
		}
        break;
    case xcldev::POWER:
    	if (ipmask == xcldev::POWER_NONE_MASK) {
    		result = -1;
    	}
    	if (ipmask == xcldev::POWER_ONCE_MASK) {
    		std::cout << "power once running" << std::endl;
    		result = deviceVec[index]->readPowerOnce();
    	}
    	if (ipmask == xcldev::POWER_TRACE_MASK) {
    		std::cout << "power trace running" << std::endl;
    		result = deviceVec[index]->readPowerTrace(sampleFreq, powerTraceFile);
    	}
    	break;
    default:
        std::cout << "ERROR: Not implemented\n";
        result = -1;
    }

    if(result == 0) {
        std::cout << "INFO: xbsak " << v->first << " successful." << std::endl;
    } else {
        std::cout << "ERROR: xbsak " << v->first  << " failed." << std::endl;
    }

    return result;
}

void xcldev::printHelp(const std::string& exe)
{
    std::cout << "Running xbsak for 4.0+ DSA's \n\n";
    std::cout << "Usage: " << exe << " <command> [options]\n\n";
    std::cout << "Command and option summary:\n";
    std::cout << "  boot    [-d device]\n";
    std::cout << "  clock   [-d device] [-r region] [-f clock1_freq_MHz] [-g clock2_freq_MHz]\n";
    std::cout << "  dmatest [-d device] [-b [0x]block_size_KB]\n";
    std::cout << "  mem     --read [-d device] [-a [0x]start_addr] [-i size_bytes] [-o output filename]\n";
    std::cout << "  mem     --write [-d device] [-a [0x]start_addr] [-i size_bytes] [-e pattern_byte]\n";
//    std::cout << "  dd      [-d device] [--if=input file] [--bs=block size] [--count=block count] [--seek=destination offset in blocks]\n";
//    std::cout << "  dd      [-d device] [--of=output file] [--bs=block size] [--count=block count] [--skip=source offset in blocks]\n";
    //        std::cout << "  fan     [-d device] -s speed\n";
    std::cout << "  flash   [-d device] -m primary_mcs [-n secondary_mcs] [-o bpi|spi]\n";
    std::cout << "  flash   scan\n";
    std::cout << "  help\n";
    std::cout << "  list\n";
    std::cout << "  scan\n";
    std::cout << "  program [-d device] [-r region] -p xclbin\n";
    std::cout << "  query   [-d device [-r region]]\n";
    std::cout << "  reset   [-d device] [-h | -r region]\n";
    std::cout << "  status  [--debug_ip_name]\n";
    //        std::cout << "  run     -d device [-r region] -c compunit\n"; TODO
    std::cout << "\nExamples:\n";
    std::cout << "List all devices\n";
    std::cout << "  " << exe << " list\n";
    std::cout << "Scan for Xilinx PCIe device(s) & associated drivers (if any) and relevant system information\n";
    std::cout << "  " << exe << " scan\n";
    std::cout << "Boot device 1 from PROM and retrain the PCIe link without rebooting the host\n";
    std::cout << "  " << exe << " boot -d 1\n";
    std::cout << "Change the clock frequency of region 0 in device 0 to 100 MHz\n";
    std::cout << "  " << exe << " clock -f 100\n";
    std::cout << "For device 0 which supports multiple clocks, change the clock 1 to 200MHz and clock 2 to 250MHz\n";
    std::cout << "  " << exe << " clock -f 200 -g 250\n";
    std::cout << "Download the accelerator program for device 2\n";
    std::cout << "  " << exe << " program -d 2 -p a.xclbin\n";
    std::cout << "Run DMA test on device 1 with 32 KB blocks of buffer\n";
    std::cout << "  " << exe << " dmatest -d 1 -b 0x2000\n";
    std::cout << "Read 256 bytes from DDR starting at 0x1000 into file read.out\n";
    std::cout << "  " << exe << " mem --read -a 0x1000 -i 256 -o read.out\n";
    std::cout << "  " << "Default values for address is 0x0, size is DDR size and file is memread.out\n";
    std::cout << "Write 256 bytes to DDR starting at 0x1000 with byte 0xaa \n";
    std::cout << "  " << exe << " mem --write -a 0x1000 -i 256 -e 0xaa\n";
    std::cout << "  " << "Default values for address is 0x0, size is DDR size and pattern is 0x0\n";
//    std::cout << "Write 2048 bytes from file to device in 1024 byte blocks starting at 0x400\n";
//    std::cout << "  " << exe << " dd -d0 --if=in.txt --bs=1024 --count=2 --seek=1\n";
//    std::cout << "Write 512 bytes from device to file in 16 byte blocks starting at 0x400\n";
//    std::cout << "  " << exe << " dd -d0 --of=out.txt --bs=16 --count=32 --skip=64\n";
    std::cout << "List the debug IPs available on the platform\n";
    std::cout << "  " << exe << " status \n";
}
