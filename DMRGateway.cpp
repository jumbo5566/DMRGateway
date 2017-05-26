/*
 *   Copyright (C) 2015,2016,2017 by Jonathan Naylor G4KLX
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "RewriteType.h"
#include "RewriteSrc.h"
#include "DMRGateway.h"
#include "StopWatch.h"
#include "RewritePC.h"
#include "Version.h"
#include "Thread.h"
#include "Voice.h"
#include "Log.h"
#include "GitVersion.h"

#include <cstdio>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "DMRGateway.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/DMRGateway.ini";
#endif

const unsigned int XLX_SLOT = 2U;
const unsigned int XLX_TG   = 9U;

static bool m_killed = false;
static int  m_signal = 0;

#if !defined(_WIN32) && !defined(_WIN64)
static void sigHandler(int signum)
{
  m_killed = true;
  m_signal = signum;
}
#endif

enum DMRGW_STATUS {
	DMRGWS_NONE,
	DMRGWS_DMRNETWORK1,
	DMRGWS_DMRNETWORK2,
	DMRGWS_XLXREFLECTOR1,
	DMRGWS_XLXREFLECTOR2
};

const char* HEADER1 = "This software is for use on amateur radio networks only,";
const char* HEADER2 = "it is to be used for educational purposes only. Its use on";
const char* HEADER3 = "commercial networks is strictly prohibited.";
const char* HEADER4 = "Copyright(C) 2017 by Jonathan Naylor, G4KLX and others";

int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;

	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "DMRGateway version %s git #%.7s\n", VERSION, gitversion);
				return 0;
			} else if (arg.substr(0,1) == "-") {
				::fprintf(stderr, "Usage: DMRGateway [-v|--version] [filename]\n");
				return 1;
			} else {
				iniFile = argv[currentArg];
			}
		}
	}

#if !defined(_WIN32) && !defined(_WIN64)
	::signal(SIGTERM, sigHandler);
	::signal(SIGHUP,  sigHandler);
#endif

	int ret = 0;

	do {
		m_signal = 0;

		CDMRGateway* host = new CDMRGateway(std::string(iniFile));
		ret = host->run();

		delete host;

		if (m_signal == 15)
			::LogInfo("Caught SIGTERM, exiting");

		if (m_signal == 1)
			::LogInfo("Caught SIGHUP, restarting");
	} while (m_signal == 1);

	::LogFinalise();

	return ret;
}

CDMRGateway::CDMRGateway(const std::string& confFile) :
m_conf(confFile),
m_repeater(NULL),
m_dmrNetwork1(NULL),
m_dmrNetwork2(NULL),
m_xlxNetwork1(NULL),
m_xlxNetwork2(NULL),
m_xlx1Reflector(4000U),
m_xlx1Slot(0U),
m_xlx1TG(0U),
m_xlx1Base(0U),
m_rpt1Rewrite(NULL),
m_xlx1Rewrite(NULL),
m_xlx2Reflector(4000U),
m_xlx2Slot(0U),
m_xlx2TG(0U),
m_xlx2Base(0U),
m_rpt2Rewrite(NULL),
m_xlx2Rewrite(NULL),
m_dmr1NetRewrites(),
m_dmr1RFRewrites(),
m_dmr2NetRewrites(),
m_dmr2RFRewrites()
{
}

CDMRGateway::~CDMRGateway()
{
	for (std::vector<IRewrite*>::iterator it = m_dmr1NetRewrites.begin(); it != m_dmr1NetRewrites.end(); ++it)
		delete *it;

	for (std::vector<IRewrite*>::iterator it = m_dmr1RFRewrites.begin(); it != m_dmr1RFRewrites.end(); ++it)
		delete *it;
	
	for (std::vector<IRewrite*>::iterator it = m_dmr2NetRewrites.begin(); it != m_dmr2NetRewrites.end(); ++it)
			delete *it;
	
	for (std::vector<IRewrite*>::iterator it = m_dmr2RFRewrites.begin(); it != m_dmr2RFRewrites.end(); ++it)
			delete *it;

	delete m_rpt1Rewrite;
	delete m_xlx1Rewrite;
	delete m_rpt2Rewrite;
	delete m_xlx2Rewrite;
}

int CDMRGateway::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "DMRGateway: cannot read the .ini file\n");
		return 1;
	}

	ret = ::LogInitialise(m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel());
	if (!ret) {
		::fprintf(stderr, "DMRGateway: unable to open the log file\n");
		return 1;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::LogWarning("Couldn't fork() , exiting");
			return -1;
		} else if (pid != 0) {
			exit(EXIT_SUCCESS);
		}

		// Create new session and process group
		if (::setsid() == -1){
			::LogWarning("Couldn't setsid(), exiting");
			return -1;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1){
			::LogWarning("Couldn't cd /, exiting");
			return -1;
		}

		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);

		//If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::LogError("Could not get the mmdvm user, exiting");
				return -1;
			}
			
			uid_t mmdvm_uid = user->pw_uid;
		    gid_t mmdvm_gid = user->pw_gid;

		    //Set user and group ID's to mmdvm:mmdvm
		    if (setgid(mmdvm_gid) != 0) {
			    ::LogWarning("Could not set mmdvm GID, exiting");
			    return -1;
		    }

			if (setuid(mmdvm_uid) != 0) {
			    ::LogWarning("Could not set mmdvm UID, exiting");
			    return -1;
		    }
		    
		    //Double check it worked (AKA Paranoia) 
		    if (setuid(0) != -1){
			    ::LogWarning("It's possible to regain root - something is wrong!, exiting");
			    return -1;
		    }
		
		}
	}
#endif

	LogInfo(HEADER1);
	LogInfo(HEADER2);
	LogInfo(HEADER3);
	LogInfo(HEADER4);

	LogMessage("DMRGateway-%s is starting", VERSION);
	LogMessage("Built %s %s (GitID #%.7s)", __TIME__, __DATE__, gitversion);

	ret = createMMDVM();
	if (!ret)
		return 1;

	LogMessage("Waiting for MMDVM to connect.....");

	for (;;) {
		unsigned char config[400U];
		unsigned int len = m_repeater->getConfig(config);
		if (len > 0U)
			break;

		m_repeater->clock(10U);

		CThread::sleep(10U);
	}

	LogMessage("MMDVM has connected");

	if (m_conf.getDMRNetwork1Enabled()) {
		ret = createDMRNetwork1();
		if (!ret)
			return 1;
	}

	if (m_conf.getDMRNetwork2Enabled()) {
		ret = createDMRNetwork2();
		if (!ret)
			return 1;
	}


	if (m_conf.getXLXNetwork1Enabled()) {
		ret = createXLXNetwork1();
		if (!ret)
			return 1;
	}

	if (m_conf.getXLXNetwork2Enabled()) {
		ret = createXLXNetwork2();
		if (!ret)
			return 1;
	}

	unsigned int timeout = m_conf.getTimeout();

	CVoice* voice1 = NULL;
	CVoice* voice2 = NULL;
	if (m_conf.getVoiceEnabled() && (m_xlxNetwork1 != NULL || m_xlxNetwork2 != NULL)) {
		std::string language  = m_conf.getVoiceLanguage();
		std::string directory = m_conf.getVoiceDirectory();

		LogInfo("Voice Parameters");
		LogInfo("    Enabled: yes");
		LogInfo("    Language: %s", language.c_str());
		LogInfo("    Directory: %s", directory.c_str());

		if (m_xlxNetwork1 != NULL) {
			voice1 = new CVoice(directory, language, m_repeater->getId(), m_xlx1Slot, m_xlx1TG);
			bool ret = voice1->open();
			if (!ret) {
				delete voice1;
				voice1 = NULL;
			}
		}

		if (m_xlxNetwork2 != NULL) {
			voice2 = new CVoice(directory, language, m_repeater->getId(), m_xlx2Slot, m_xlx2TG);
			bool ret = voice2->open();
			if (!ret) {
				delete voice2;
				voice2 = NULL;
			}
		}
	}

	CTimer* timer[3U];
	timer[1U] = new CTimer(1000U, timeout);
	timer[2U] = new CTimer(1000U, timeout);

	DMRGW_STATUS status[3U];
	status[1U] = DMRGWS_NONE;
	status[2U] = DMRGWS_NONE;

	CStopWatch stopWatch;
	stopWatch.start();

	LogMessage("DMRGateway-%s is running", VERSION);

	bool changed = false;

	while (!m_killed) {
		CDMRData data;

		bool ret = m_repeater->read(data);
		if (ret) {
			unsigned int slotNo = data.getSlotNo();
			unsigned int dstId = data.getDstId();
			FLCO flco = data.getFLCO();

			if (flco == FLCO_GROUP && slotNo == m_xlx1Slot && dstId == m_xlx1TG) {
				m_xlx1Rewrite->process(data);
				m_xlxNetwork1->write(data);
				status[slotNo] = DMRGWS_XLXREFLECTOR1;
				timer[slotNo]->start();
			} else if (flco == FLCO_GROUP && slotNo == m_xlx2Slot && dstId == m_xlx2TG) {
				m_xlx2Rewrite->process(data);
				m_xlxNetwork2->write(data);
				status[slotNo] = DMRGWS_XLXREFLECTOR2;
				timer[slotNo]->start();
			} else if (flco == FLCO_USER_USER && slotNo == m_xlx1Slot && dstId >= m_xlx1Base && dstId <= (m_xlx1Base + 26U)) {
				dstId += 4000U;
				dstId -= m_xlx1Base;

				if (dstId != m_xlx1Reflector) {
					if (dstId == 4000U)
						LogMessage("XLX-1, Unlinking");
					else
						LogMessage("XLX-1, Linking to reflector %u", dstId);

					m_xlx1Reflector = dstId;
					changed = true;
				}

				data.setSlotNo(XLX_SLOT);
				m_xlxNetwork1->write(data);
				status[slotNo] = DMRGWS_XLXREFLECTOR1;
				timer[slotNo]->start();

				if (voice1 != NULL) {
					unsigned char type = data.getDataType();
					if (type == DT_TERMINATOR_WITH_LC) {
						if (changed) {
							if (m_xlx1Reflector == 4000U)
								voice1->unlinked();
							else
								voice1->linkedTo(m_xlx1Reflector);
							changed = false;
						}
					}
				}
			} else if (flco == FLCO_USER_USER && slotNo == m_xlx2Slot && dstId >= m_xlx2Base && dstId <= (m_xlx2Base + 26U)) {
				dstId += 4000U;
				dstId -= m_xlx2Base;

				if (dstId != m_xlx1Reflector) {
					if (dstId == 4000U)
						LogMessage("XLX-2, Unlinking");
					else
						LogMessage("XLX-2, Linking to reflector %u", dstId);

					m_xlx2Reflector = dstId;
					changed = true;
				}

				data.setSlotNo(XLX_SLOT);
				m_xlxNetwork2->write(data);
				status[slotNo] = DMRGWS_XLXREFLECTOR2;
				timer[slotNo]->start();

				if (voice2 != NULL) {
					unsigned char type = data.getDataType();
					if (type == DT_TERMINATOR_WITH_LC) {
						if (changed) {
							if (m_xlx2Reflector == 4000U)
								voice2->unlinked();
							else
								voice2->linkedTo(m_xlx2Reflector);
							changed = false;
						}
					}
				}
			} else {
				bool rewritten = false;

				if (m_dmrNetwork1 != NULL) {
					// Rewrite the slot and/or TG or neither
					for (std::vector<IRewrite*>::iterator it = m_dmr1RFRewrites.begin(); it != m_dmr1RFRewrites.end(); ++it) {
						bool ret = (*it)->process(data);
						if (ret) {
							rewritten = true;
							break;
						}
					}

					if (rewritten) {
						unsigned int slotNo = data.getSlotNo();
						if (status[slotNo] == DMRGWS_NONE || status[slotNo] == DMRGWS_DMRNETWORK1) {
							m_dmrNetwork1->write(data);
							status[slotNo] = DMRGWS_DMRNETWORK1;
							timer[slotNo]->start();
						}
					}
				}

				if (!rewritten) {
					if (m_dmrNetwork2 != NULL) {
						// Rewrite the slot and/or TG or neither
						for (std::vector<IRewrite*>::iterator it = m_dmr2RFRewrites.begin(); it != m_dmr2RFRewrites.end(); ++it) {
							bool ret = (*it)->process(data);
							if (ret) {
								rewritten = true;
								break;
							}
						}

						if (rewritten) {
							unsigned int slotNo = data.getSlotNo();
							if (status[slotNo] == DMRGWS_NONE || status[slotNo] == DMRGWS_DMRNETWORK2) {
								m_dmrNetwork2->write(data);
								status[slotNo] = DMRGWS_DMRNETWORK2;
								timer[slotNo]->start();
							}
						}
					}
				}
			}
		}

		if (m_xlxNetwork1 != NULL) {
			ret = m_xlxNetwork1->read(data);
			if (ret) {
				if (status[m_xlx1Slot] == DMRGWS_NONE || status[m_xlx1Slot] == DMRGWS_XLXREFLECTOR1) {
					bool ret = m_rpt1Rewrite->process(data);
					if (ret) {
						m_repeater->write(data);
						status[m_xlx1Slot] = DMRGWS_XLXREFLECTOR1;
						timer[m_xlx1Slot]->start();
					} else {
						unsigned int slotNo = data.getSlotNo();
						unsigned int dstId  = data.getDstId();
						FLCO flco           = data.getFLCO();
						LogWarning("XLX-1, Unexpected data from slot %u %s%u", slotNo, flco == FLCO_GROUP ? "TG" : "", dstId);
					}
				}
			}
		}

		if (m_xlxNetwork2 != NULL) {
			ret = m_xlxNetwork2->read(data);
			if (ret) {
				if (status[m_xlx2Slot] == DMRGWS_NONE || status[m_xlx2Slot] == DMRGWS_XLXREFLECTOR2) {
					bool ret = m_rpt2Rewrite->process(data);
					if (ret) {
						m_repeater->write(data);
						status[m_xlx2Slot] = DMRGWS_XLXREFLECTOR2;
						timer[m_xlx2Slot]->start();
					}
					else {
						unsigned int slotNo = data.getSlotNo();
						unsigned int dstId = data.getDstId();
						FLCO flco = data.getFLCO();
						LogWarning("XLX-2, Unexpected data from slot %u %s%u", slotNo, flco == FLCO_GROUP ? "TG" : "", dstId);
					}
				}
			}
		}

		if (m_dmrNetwork1 != NULL) {
			ret = m_dmrNetwork1->read(data);
			if (ret) {
				// Rewrite the slot and/or TG or neither
				bool rewritten = false;
				for (std::vector<IRewrite*>::iterator it = m_dmr1NetRewrites.begin(); it != m_dmr1NetRewrites.end(); ++it) {
					bool ret = (*it)->process(data);
					if (ret) {
						rewritten = true;
						break;
					}
				}

				if (rewritten) {
					unsigned int slotNo = data.getSlotNo();
					unsigned int dstId  = data.getDstId();
					FLCO flco           = data.getFLCO();

					if (status[slotNo] == DMRGWS_NONE || status[slotNo] == DMRGWS_DMRNETWORK1) {
						m_repeater->write(data);
						status[slotNo] = DMRGWS_DMRNETWORK1;
						timer[slotNo]->start();
					}
				}
			}
		}

		if (m_dmrNetwork2 != NULL) {
			ret = m_dmrNetwork2->read(data);
			if (ret) {
				// Rewrite the slot and/or TG or neither
				bool rewritten = false;
				for (std::vector<IRewrite*>::iterator it = m_dmr2NetRewrites.begin(); it != m_dmr2NetRewrites.end(); ++it) {
					bool ret = (*it)->process(data);
					if (ret) {
						rewritten = true;
						break;
					}
				}

				if (rewritten) {
					unsigned int slotNo = data.getSlotNo();
					unsigned int dstId  = data.getDstId();
					FLCO flco           = data.getFLCO();

					if (status[slotNo] == DMRGWS_NONE || status[slotNo] == DMRGWS_DMRNETWORK2) {
						m_repeater->write(data);
						status[slotNo] = DMRGWS_DMRNETWORK2;
						timer[slotNo]->start();
					}
				}
			}
		}

		unsigned char buffer[50U];
		unsigned int length;
		ret = m_repeater->readPosition(buffer, length);
		if (ret) {
			if (m_xlxNetwork1 != NULL)
				m_xlxNetwork1->writePosition(buffer, length);
			if (m_xlxNetwork2 != NULL)
				m_xlxNetwork2->writePosition(buffer, length);
			if (m_dmrNetwork1 != NULL)
				m_dmrNetwork1->writePosition(buffer, length);
			if (m_dmrNetwork2 != NULL)
				m_dmrNetwork2->writePosition(buffer, length);
		}
		ret = m_repeater->readTalkerAlias(buffer, length);
		if (ret) {
			if (m_xlxNetwork1 != NULL)
				m_xlxNetwork1->writeTalkerAlias(buffer, length);
			if (m_xlxNetwork2 != NULL)
				m_xlxNetwork2->writeTalkerAlias(buffer, length);
			if (m_dmrNetwork1 != NULL)
				m_dmrNetwork1->writeTalkerAlias(buffer, length);
			if (m_dmrNetwork2 != NULL)
				m_dmrNetwork2->writeTalkerAlias(buffer, length);
		}

		if (voice1 != NULL) {
			ret = voice1->read(data);
			if (ret) {
				m_repeater->write(data);
				status[m_xlx1Slot] = DMRGWS_XLXREFLECTOR1;
				timer[m_xlx1Slot]->start();
			}
		}

		if (voice2 != NULL) {
			ret = voice2->read(data);
			if (ret) {
				m_repeater->write(data);
				status[m_xlx2Slot] = DMRGWS_XLXREFLECTOR2;
				timer[m_xlx2Slot]->start();
			}
		}

		unsigned int ms = stopWatch.elapsed();
		stopWatch.start();

		m_repeater->clock(ms);

		if (m_dmrNetwork1 != NULL)
			m_dmrNetwork1->clock(ms);

		if (m_dmrNetwork2 != NULL)
			m_dmrNetwork2->clock(ms);

		if (m_xlxNetwork1 != NULL)
			m_xlxNetwork1->clock(ms);

		if (m_xlxNetwork2 != NULL)
			m_xlxNetwork2->clock(ms);

		if (voice1 != NULL)
			voice1->clock(ms);

		if (voice2 != NULL)
			voice2->clock(ms);

		for (unsigned int i = 1U; i < 3U; i++) {
			timer[i]->clock(ms);
			if (timer[i]->isRunning() && timer[i]->hasExpired()) {
				status[i] = DMRGWS_NONE;
				timer[i]->stop();
			}
		}

		if (ms < 10U)
			CThread::sleep(10U);
	}

	LogMessage("DMRGateway-%s is exiting on receipt of SIGHUP1", VERSION);

	delete voice1;
	delete voice2;

	m_repeater->close();
	delete m_repeater;

	if (m_dmrNetwork1 != NULL) {
		m_dmrNetwork1->close();
		delete m_dmrNetwork1;
	}

	if (m_dmrNetwork2 != NULL) {
		m_dmrNetwork2->close();
		delete m_dmrNetwork2;
	}

	if (m_xlxNetwork1 != NULL) {
		m_xlxNetwork1->close();
		delete m_xlxNetwork1;
	}

	if (m_xlxNetwork2 != NULL) {
		m_xlxNetwork2->close();
		delete m_xlxNetwork2;
	}

	delete timer[1U];
	delete timer[2U];

	return 0;
}

bool CDMRGateway::createMMDVM()
{
	std::string rptAddress   = m_conf.getRptAddress();
	unsigned int rptPort     = m_conf.getRptPort();
	std::string localAddress = m_conf.getLocalAddress();
	unsigned int localPort   = m_conf.getLocalPort();
	bool debug               = m_conf.getDebug();

	LogInfo("MMDVM Network Parameters");
	LogInfo("    Rpt Address: %s", rptAddress.c_str());
	LogInfo("    Rpt Port: %u", rptPort);
	LogInfo("    Local Address: %s", localAddress.c_str());
	LogInfo("    Local Port: %u", localPort);

	m_repeater = new CMMDVMNetwork(rptAddress, rptPort, localAddress, localPort, debug);

	bool ret = m_repeater->open();
	if (!ret) {
		delete m_repeater;
		m_repeater = NULL;
		return false;
	}

	return true;
}

bool CDMRGateway::createDMRNetwork1()
{
	std::string address  = m_conf.getDMRNetwork1Address();
	unsigned int port    = m_conf.getDMRNetwork1Port();
	unsigned int local   = m_conf.getDMRNetwork1Local();
	unsigned int id      = m_conf.getDMRNetwork1Id();
	std::string password = m_conf.getDMRNetwork1Password();
	bool debug           = m_conf.getDMRNetwork1Debug();

	if (id == 0U)
		id = m_repeater->getId();

	LogInfo("DMR Network 1 Parameters");
	LogInfo("    Id: %u", id);
	LogInfo("    Address: %s", address.c_str());
	LogInfo("    Port: %u", port);
	if (local > 0U)
		LogInfo("    Local: %u", local);
	else
		LogInfo("    Local: random");

	m_dmrNetwork1 = new CDMRNetwork(address, port, local, id, password, "DMR-1", debug);

	std::string options = m_conf.getDMRNetwork1Options();
	if (options.empty())
		options = m_repeater->getOptions();

	if (!options.empty()) {
		LogInfo("    Options: %s", options.c_str());
		m_dmrNetwork1->setOptions(options);
	}

	unsigned char config[400U];
	unsigned int len = m_repeater->getConfig(config);

	m_dmrNetwork1->setConfig(config, len);

	bool ret = m_dmrNetwork1->open();
	if (!ret) {
		delete m_dmrNetwork1;
		m_dmrNetwork1 = NULL;
		return false;
	}

	std::vector<CTGRewriteStruct> tgRewrites = m_conf.getDMRNetwork1TGRewrites();
	for (std::vector<CTGRewriteStruct>::const_iterator it = tgRewrites.begin(); it != tgRewrites.end(); ++it) {
		LogInfo("    Rewrite RF: %u:TG%u-TG%u -> %u:TG%u-TG%u", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_fromTG + (*it).m_range - 1U, (*it).m_toSlot, (*it).m_toTG, (*it).m_toTG + (*it).m_range - 1U);
		LogInfo("    Rewrite Net: %u:TG%u-TG%u -> %u:TG%u-TG%u", (*it).m_toSlot, (*it).m_toTG, (*it).m_toTG + (*it).m_range - 1U, (*it).m_fromSlot, (*it).m_fromTG, (*it).m_fromTG + (*it).m_range - 1U);

		CRewriteTG* rfRewrite  = new CRewriteTG("DMR-1", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_toSlot, (*it).m_toTG, (*it).m_range);
		CRewriteTG* netRewrite = new CRewriteTG("DMR-1", (*it).m_toSlot, (*it).m_toTG, (*it).m_fromSlot, (*it).m_fromTG, (*it).m_range);

		m_dmr1RFRewrites.push_back(rfRewrite);
		m_dmr1NetRewrites.push_back(netRewrite);
	}

	std::vector<CPCRewriteStruct> pcRewrites = m_conf.getDMRNetwork1PCRewrites();
	for (std::vector<CPCRewriteStruct>::const_iterator it = pcRewrites.begin(); it != pcRewrites.end(); ++it) {
		LogInfo("    Rewrite RF: %u:%u-%u -> %u:%u-%u", (*it).m_fromSlot, (*it).m_fromId, (*it).m_fromId + (*it).m_range - 1U, (*it).m_toSlot, (*it).m_toId, (*it).m_toId + (*it).m_range - 1U);

		CRewritePC* rewrite = new CRewritePC("DMR-1", (*it).m_fromSlot, (*it).m_fromId, (*it).m_toSlot, (*it).m_toId, (*it).m_range);

		m_dmr1RFRewrites.push_back(rewrite);
	}

	std::vector<CTypeRewriteStruct> typeRewrites = m_conf.getDMRNetwork1TypeRewrites();
	for (std::vector<CTypeRewriteStruct>::const_iterator it = typeRewrites.begin(); it != typeRewrites.end(); ++it) {
		LogInfo("    Rewrite RF: %u:TG%u -> %u:%u", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_toSlot, (*it).m_toId);

		CRewriteType* rewrite = new CRewriteType("DMR-1", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_toSlot, (*it).m_toId);

		m_dmr1RFRewrites.push_back(rewrite);
	}

	std::vector<CSrcRewriteStruct> srcRewrites = m_conf.getDMRNetwork1SrcRewrites();
	for (std::vector<CSrcRewriteStruct>::const_iterator it = srcRewrites.begin(); it != srcRewrites.end(); ++it) {
		LogInfo("    Rewrite Net: %u:%u-%u -> %u:TG%u", (*it).m_fromSlot, (*it).m_fromId, (*it).m_fromId + (*it).m_range - 1U, (*it).m_toSlot, (*it).m_toTG);

		CRewriteSrc* rewrite = new CRewriteSrc("DMR-1", (*it).m_fromSlot, (*it).m_fromId, (*it).m_toSlot, (*it).m_toTG, (*it).m_range);

		m_dmr1NetRewrites.push_back(rewrite);
	}

	return true;
}

bool CDMRGateway::createDMRNetwork2()
{
	std::string address  = m_conf.getDMRNetwork2Address();
	unsigned int port    = m_conf.getDMRNetwork2Port();
	unsigned int local   = m_conf.getDMRNetwork2Local();
	unsigned int id      = m_conf.getDMRNetwork2Id();
	std::string password = m_conf.getDMRNetwork2Password();
	bool debug           = m_conf.getDMRNetwork2Debug();

	if (id == 0U)
		id = m_repeater->getId();

	LogInfo("DMR Network 2 Parameters");
	LogInfo("    Id: %u", id);
	LogInfo("    Address: %s", address.c_str());
	LogInfo("    Port: %u", port);
	if (local > 0U)
		LogInfo("    Local: %u", local);
	else
		LogInfo("    Local: random");

	m_dmrNetwork2 = new CDMRNetwork(address, port, local, id, password, "DMR-2", debug);

	std::string options = m_conf.getDMRNetwork2Options();
	if (options.empty())
		options = m_repeater->getOptions();

	if (!options.empty()) {
		LogInfo("    Options: %s", options.c_str());
		m_dmrNetwork2->setOptions(options);
	}

	unsigned char config[400U];
	unsigned int len = m_repeater->getConfig(config);

	m_dmrNetwork2->setConfig(config, len);

	bool ret = m_dmrNetwork2->open();
	if (!ret) {
		delete m_dmrNetwork2;
		m_dmrNetwork2 = NULL;
		return false;
	}

	std::vector<CTGRewriteStruct> tgRewrites = m_conf.getDMRNetwork2TGRewrites();
	for (std::vector<CTGRewriteStruct>::const_iterator it = tgRewrites.begin(); it != tgRewrites.end(); ++it) {
		LogInfo("    Rewrite RF: %u:TG%u-TG%u -> %u:TG%u-TG%u", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_fromTG + (*it).m_range - 1U, (*it).m_toSlot, (*it).m_toTG, (*it).m_toTG + (*it).m_range - 1U);
		LogInfo("    Rewrite Net: %u:TG%u-TG%u -> %u:TG%u-TG%u", (*it).m_toSlot, (*it).m_toTG, (*it).m_toTG + (*it).m_range - 1U, (*it).m_fromSlot, (*it).m_fromTG, (*it).m_fromTG + (*it).m_range - 1U);

		CRewriteTG* rfRewrite  = new CRewriteTG("DMR-2", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_toSlot, (*it).m_toTG, (*it).m_range);
		CRewriteTG* netRewrite = new CRewriteTG("DMR-2", (*it).m_toSlot, (*it).m_toTG, (*it).m_fromSlot, (*it).m_fromTG, (*it).m_range);

		m_dmr2RFRewrites.push_back(rfRewrite);
		m_dmr2NetRewrites.push_back(netRewrite);
	}

	std::vector<CPCRewriteStruct> pcRewrites = m_conf.getDMRNetwork2PCRewrites();
	for (std::vector<CPCRewriteStruct>::const_iterator it = pcRewrites.begin(); it != pcRewrites.end(); ++it) {
		LogInfo("    Rewrite RF: %u:%u-%u -> %u:%u-%u", (*it).m_fromSlot, (*it).m_fromId, (*it).m_fromId + (*it).m_range - 1U, (*it).m_toSlot, (*it).m_toId, (*it).m_toId + (*it).m_range - 1U);

		CRewritePC* rewrite = new CRewritePC("DMR-2", (*it).m_fromSlot, (*it).m_fromId, (*it).m_toSlot, (*it).m_toId, (*it).m_range);

		m_dmr2RFRewrites.push_back(rewrite);
	}

	std::vector<CTypeRewriteStruct> typeRewrites = m_conf.getDMRNetwork2TypeRewrites();
	for (std::vector<CTypeRewriteStruct>::const_iterator it = typeRewrites.begin(); it != typeRewrites.end(); ++it) {
		LogInfo("    Rewrite RF: %u:TG%u -> %u:%u", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_toSlot, (*it).m_toId);

		CRewriteType* rewrite = new CRewriteType("DMR-2", (*it).m_fromSlot, (*it).m_fromTG, (*it).m_toSlot, (*it).m_toId);

		m_dmr2RFRewrites.push_back(rewrite);
	}

	std::vector<CSrcRewriteStruct> srcRewrites = m_conf.getDMRNetwork2SrcRewrites();
	for (std::vector<CSrcRewriteStruct>::const_iterator it = srcRewrites.begin(); it != srcRewrites.end(); ++it) {
		LogInfo("    Rewrite Net: %u:%u-%u -> %u:TG%u", (*it).m_fromSlot, (*it).m_fromId, (*it).m_fromId + (*it).m_range - 1U, (*it).m_toSlot, (*it).m_toTG);

		CRewriteSrc* rewrite = new CRewriteSrc("DMR-2", (*it).m_fromSlot, (*it).m_fromId, (*it).m_toSlot, (*it).m_toTG, (*it).m_range);

		m_dmr2NetRewrites.push_back(rewrite);
	}

	return true;
}

bool CDMRGateway::createXLXNetwork1()
{
	std::string address  = m_conf.getXLXNetwork1Address();
	unsigned int port    = m_conf.getXLXNetwork1Port();
	unsigned int local   = m_conf.getXLXNetwork1Local();
	unsigned int id      = m_conf.getXLXNetwork1Id();
	std::string password = m_conf.getXLXNetwork1Password();
	bool debug           = m_conf.getXLXNetwork1Debug();

	if (id == 0U)
		id = m_repeater->getId();

	LogInfo("XLX Network 1 Parameters");
	LogInfo("    Id: %u", id);
	LogInfo("    Address: %s", address.c_str());
	LogInfo("    Port: %u", port);
	if (local > 0U)
		LogInfo("    Local: %u", local);
	else
		LogInfo("    Local: random");

	m_xlxNetwork1 = new CDMRNetwork(address, port, local, id, password, "XLX-1", debug);

	std::string options = m_conf.getXLXNetwork1Options();
	if (!options.empty()) {
		LogInfo("    Options: %s", options.c_str());
		m_xlxNetwork1->setOptions(options);
	}

	unsigned char config[400U];
	unsigned int len = m_repeater->getConfig(config);

	m_xlxNetwork1->setConfig(config, len);

	bool ret = m_xlxNetwork1->open();
	if (!ret) {
		delete m_xlxNetwork1;
		m_xlxNetwork1 = NULL;
		return false;
	}

	m_xlx1Slot = m_conf.getXLXNetwork1Slot();
	m_xlx1TG   = m_conf.getXLXNetwork1TG();
	m_xlx1Base = m_conf.getXLXNetwork1Base();

	LogInfo("    Slot: %u", m_xlx1Slot);
	LogInfo("    TG: %u", m_xlx1TG);
	LogInfo("    Base: %u", m_xlx1Base);

	m_rpt1Rewrite = new CRewriteTG("XLX-1", XLX_SLOT, XLX_TG, m_xlx1Slot, m_xlx1TG, 1U);
	m_xlx1Rewrite = new CRewriteTG("XLX-1", m_xlx1Slot, m_xlx1TG, XLX_SLOT, XLX_TG, 1U);

	return true;
}

bool CDMRGateway::createXLXNetwork2()
{
	std::string address  = m_conf.getXLXNetwork2Address();
	unsigned int port    = m_conf.getXLXNetwork2Port();
	unsigned int local   = m_conf.getXLXNetwork2Local();
	unsigned int id      = m_conf.getXLXNetwork2Id();
	std::string password = m_conf.getXLXNetwork2Password();
	bool debug           = m_conf.getXLXNetwork2Debug();

	if (id == 0U)
		id = m_repeater->getId();

	LogInfo("XLX Network 2 Parameters");
	LogInfo("    Id: %u", id);
	LogInfo("    Address: %s", address.c_str());
	LogInfo("    Port: %u", port);
	if (local > 0U)
		LogInfo("    Local: %u", local);
	else
		LogInfo("    Local: random");

	m_xlxNetwork2 = new CDMRNetwork(address, port, local, id, password, "XLX-2", debug);

	std::string options = m_conf.getXLXNetwork2Options();
	if (!options.empty()) {
		LogInfo("    Options: %s", options.c_str());
		m_xlxNetwork2->setOptions(options);
	}

	unsigned char config[400U];
	unsigned int len = m_repeater->getConfig(config);

	m_xlxNetwork2->setConfig(config, len);

	bool ret = m_xlxNetwork2->open();
	if (!ret) {
		delete m_xlxNetwork2;
		m_xlxNetwork2 = NULL;
		return false;
	}

	m_xlx2Slot = m_conf.getXLXNetwork2Slot();
	m_xlx2TG   = m_conf.getXLXNetwork2TG();
	m_xlx2Base = m_conf.getXLXNetwork2Base();

	LogInfo("    Slot: %u", m_xlx2Slot);
	LogInfo("    TG: %u", m_xlx2TG);
	LogInfo("    Base: %u", m_xlx2Base);

	m_rpt2Rewrite = new CRewriteTG("XLX-2", XLX_SLOT, XLX_TG, m_xlx2Slot, m_xlx2TG, 1U);
	m_xlx2Rewrite = new CRewriteTG("XLX-2", m_xlx2Slot, m_xlx2TG, XLX_SLOT, XLX_TG, 1U);

	return true;
}
