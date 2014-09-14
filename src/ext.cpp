/*
Copyright (C) 2014 Declan Ireland <http://github.com/torndeco/extDB>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "ext.h"

#include <Poco/Data/Session.h>
#include <Poco/Data/SessionPool.h>

#include <Poco/Data/MySQL/Connector.h>
#include <Poco/Data/MySQL/MySQLException.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/SQLite/SQLiteException.h>
#include <Poco/Data/ODBC/Connector.h>
#include <Poco/Data/ODBC/ODBCException.h>

#include <Poco/AutoPtr.h>
#include <Poco/DateTime.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/Exception.h>
#include <Poco/NumberFormatter.h>
#include <Poco/NumberParser.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Util/IniFileConfiguration.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread/thread.hpp>
#include <boost/random/random_device.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/regex.hpp>
#include <boost/shared_ptr.hpp>

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>

#include <cstring>

#ifdef TEST_APP
	#include <iostream>
#endif

#include "uniqueid.h"
#include "protocols/abstract_protocol.h"
#include "protocols/db_basic_v2.h"
#include "protocols/db_custom_v2.h"
#include "protocols/db_procedure_v2.h"
#include "protocols/db_raw_v2.h"
#include "protocols/db_raw_no_extra_quotes_v2.h"
#include "protocols/log.h"
#include "protocols/misc.h"


void DBPool::customizeSession (Poco::Data::Session& session)
{
	try
	{
		// This is mainly for SQLite Database Locks when its writing changes.. 
		//		i.e multi-threaded queries -> SQLite 
		session.setProperty("maxRetryAttempts", 100);  
	}
	catch (Poco::Data::NotSupportedException&)
	{
	}
}


Ext::Ext(void) {
	mgr.reset (new IdManager);
	extDB_lock = false;

	Poco::DateTime now;
	std::string log_filename = Poco::DateTimeFormatter::format(now, "%Y/%n/%d/%H-%M-%S.log");
	std::string log_relative_path = boost::filesystem::path("extDB/logs/" + log_filename).make_preferred().string();
	
	boost::log::add_common_attributes();

	boost::log::add_file_log
	(
		boost::log::keywords::auto_flush = true, 
		boost::log::keywords::file_name = log_relative_path,
		boost::log::keywords::format = "[%TimeStamp%]: ThreadID %ThreadID%: %Message%"
	);
	boost::log::core::get()->set_filter
	(
		boost::log::trivial::severity >= boost::log::trivial::info
	);
	

	bool conf_found = false;
	bool conf_randomized = false;
	
	if (boost::filesystem::exists("extdb-conf.ini"))
	{
		conf_found = true;
		pConf = (new Poco::Util::IniFileConfiguration("extdb-conf.ini"));
	}
	else
	{
		#ifdef _WIN32
			// Search for Randomize Config File -- Legacy Security Support For Arma2Servers
				// TODO: WINDOWS ONLY ifdef endif
			boost::regex expression("extdb-conf.*ini");

			for(boost::filesystem::directory_iterator it(boost::filesystem::current_path()); it !=  boost::filesystem::directory_iterator(); ++it)
			{
				if (is_regular_file(it->path()))
				{
					if(boost::regex_search(it->path().string(), expression))
					{
						conf_found = true;
						conf_randomized = true;
						pConf = (new Poco::Util::IniFileConfiguration(it->path().string()));  // Load Randomized Conf
						break;
					}
				}
			}
		#endif
	}

	BOOST_LOG_SEV(logger, boost::log::trivial::info) << "extDB: Version: " + version();
	
	if (!conf_found) 
	{
		#ifdef TESTING
			std::cout << "extDB: Unable to find extdb-conf.ini" << std::endl;
		#endif
		
		BOOST_LOG_SEV(logger, boost::log::trivial::fatal) << "extDB: Unable to find extdb-conf.ini";
		// Kill Server no config file found -- Evil
		std::exit(EXIT_FAILURE);
	}
	else
	{
		
		#ifdef TESTING
			std::cout << "extDB: Found extdb-conf.ini" << std::endl;
		#endif
		BOOST_LOG_SEV(logger, boost::log::trivial::info) << "extDB: Found extdb-conf.ini";

		steam_api_key = pConf->getString("Main.Steam_WEB_API_KEY", "");

		// Start Threads + ASIO
		max_threads = pConf->getInt("Main.Threads", 0);
		if (max_threads <= 0)
		{
			max_threads = boost::thread::hardware_concurrency();
		}
		io_work_ptr.reset(new boost::asio::io_service::work(io_service));
		for (int i = 0; i < max_threads; ++i)
		{
			threads.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
			#ifdef TESTING
				std::cout << "extDB: Creating Worker Thread +1" << std::endl ;
			#endif
			BOOST_LOG_SEV(logger, boost::log::trivial::info) << "extDB: Creating Worker Thread +1";
		}

		// Load Logging Filter Options
		#ifdef TESTING
			std::cout << "extDB: Loading Log Settings" << std::endl;
		#endif
		
		boost::log::core::get()->set_filter
		(
			boost::log::trivial::severity >= (pConf->getInt("Logging.Filter", 2))
		);

		#ifdef TESTING
//			std::cout << "extDB: Loading Rcon Settings" << std::endl;
//			rcon.init(pConf->getInt("Main.RconPort", 2302), pConf->getString("Main.RconPassword", "password"));
		#endif
		//BOOST_LOG_SEV(logger, boost::log::trivial::info) << "extDB: Loading Rcon Settings";
		
		#ifdef _WIN32
			if ((pConf->getBool("Main.Randomize Config File", false)) && (!conf_randomized))
			// Only Gonna Randomize Once, Keeps things Simple
			{
				std::string chars("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
								  "1234567890");
				// Skipping Lowercase, this function only for arma2 + extensions only available on windows.
				boost::random::random_device rng;
				boost::random::uniform_int_distribution<> index_dist(0, chars.size() - 1);
				
				std::string randomized_filename = "extdb-conf-";
				for(int i = 0; i < 8; ++i) {
					randomized_filename += chars[index_dist(rng)];
				}
				randomized_filename += ".ini";
				boost::filesystem::rename("extdb-conf.ini", randomized_filename);
			}
		#endif
	}
}

Ext::~Ext(void)
{
	stop();
}

void Ext::stop()
{	
	#ifdef TESTING
		std::cout << "extDB: Stopping Please Wait ..." << std::endl;
	#endif

	io_service.stop();
	threads.join_all();
	unordered_map_protocol.clear();

	boost::log::core::get()->remove_all_sinks();
}

void Ext::connectDatabase(char *output, const int &output_size, const std::string &conf_option)
{
	// TODO ADD Code to check for database already initialized !!!!!!!!!!!
	try
	{
		if (pConf->hasOption(conf_option + ".Type"))
		{
			// Database
			db_conn_info.db_type = pConf->getString(conf_option + ".Type");
			std::string db_name = pConf->getString(conf_option + ".Name");

			db_conn_info.min_sessions = pConf->getInt(conf_option + ".minSessions", 1);
			if (db_conn_info.min_sessions <= 0)
			{
				db_conn_info.min_sessions = 1;
			}
			db_conn_info.min_sessions = pConf->getInt(conf_option + ".maxSessions", 1);
			if (db_conn_info.max_sessions <= 0)
			{
				db_conn_info.max_sessions = max_threads;
			}

			db_conn_info.idle_time = pConf->getInt(conf_option + ".idleTime");

			#ifdef TESTING
				std::cout << "extDB: Database Type: " << db_conn_info.db_type << std::endl;
			#endif
			BOOST_LOG_SEV(logger, boost::log::trivial::info) << "extDB: Database Type: " << db_conn_info.db_type;

			if ( (boost::iequals(db_conn_info.db_type, std::string("MySQL")) == 1) || (boost::iequals(db_conn_info.db_type, std::string("ODBC")) == 1) )
			{
				std::string username = pConf->getString(conf_option + ".Username");
				std::string password = pConf->getString(conf_option + ".Password");

				std::string ip = pConf->getString(conf_option + ".IP");
				std::string port = pConf->getString(conf_option + ".Port");

				db_conn_info.connection_str = "host=" + ip + ";port=" + port + ";user=" + username + ";password=" + password + ";db=" + db_name + ";auto-reconnect=true";

				if (boost::iequals(db_conn_info.db_type, std::string("MySQL")) == 1)
				{
					db_conn_info.db_type = "MySQL";
					Poco::Data::MySQL::Connector::registerConnector();
					std::string compress = pConf->getString(conf_option + ".Compress", "false");
					if (boost::iequals(compress, "true") == 1)
					{
						db_conn_info.connection_str = db_conn_info.connection_str + ";compress=true";
					}
				}
				else
				{
					db_conn_info.db_type = "ODBC";
					Poco::Data::ODBC::Connector::registerConnector();
				}

				db_pool.reset(new DBPool(db_conn_info.db_type, 
															db_conn_info.connection_str, 
															db_conn_info.min_sessions, 
															db_conn_info.max_sessions, 
															db_conn_info.idle_time));
				if (db_pool->get().isConnected())
				{
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Started" << std::endl;
					#endif
					BOOST_LOG_SEV(logger, boost::log::trivial::info) << "extDB: Database Session Pool Started";
					std::strcpy(output, "[1]");
				}
				else
				{
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Failed" << std::endl;
					#endif
					BOOST_LOG_SEV(logger, boost::log::trivial::fatal) << "extDB: Database Session Pool Failed";
					std::strcpy(output, "[0,\"Database Session Pool Failed\"]");
					db_conn_info = DBConnectionInfo();
					std::exit(EXIT_FAILURE);
				}
			}
			else if (boost::iequals(db_conn_info.db_type, "SQLite") == 1)
			{
				db_conn_info.db_type = "SQLite";
				Poco::Data::SQLite::Connector::registerConnector();

				std::string sqlite_file = boost::filesystem::path("extDB/sqlite/" + db_name).make_preferred().string();
				db_conn_info.connection_str = sqlite_file;

				db_pool.reset(new DBPool(db_conn_info.db_type, 
															db_conn_info.connection_str, 
															db_conn_info.min_sessions, 
															db_conn_info.max_sessions, 
															db_conn_info.idle_time));

				if (db_pool->get().isConnected())
				{
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Started" << std::endl;
					#endif
					BOOST_LOG_SEV(logger, boost::log::trivial::info) << "extDB: Database Session Pool Started";
					std::strcpy(output, "[1]");
				}
				else
				{
					#ifdef TESTING
						std::cout << "extDB: Database Session Pool Failed" << std::endl;
					#endif
					BOOST_LOG_SEV(logger, boost::log::trivial::warning) << "extDB: Database Session Pool Failed";
					std::strcpy(output, "[0,\"Database Session Pool Failed\"]");
					db_conn_info = DBConnectionInfo();
					std::exit(EXIT_FAILURE);
				}
			}
			else
			{
				#ifdef TESTING
					std::cout << "extDB: No Database Engine Found for " << db_name << "." << std::endl;
				#endif 
				BOOST_LOG_SEV(logger, boost::log::trivial::warning) << "extDB: No Database Engine Found for " << db_name << ".";
				std::strcpy(output, "[0,\"Unknown Database Type\"]");
				db_conn_info = DBConnectionInfo();
				std::exit(EXIT_FAILURE);
			}
		}
		else
		{
			#ifdef TESTING
				std::cout << "extDB: WARNING No Config Option Found: " << conf_option << "." << std::endl;
			#endif
			BOOST_LOG_SEV(logger, boost::log::trivial::warning) << "extDB: No Config Option Found: " << conf_option << ".";
			std::strcpy(output, "[0,\"No Config Option Found\"]");
			db_conn_info = DBConnectionInfo();
			std::exit(EXIT_FAILURE);
		}
	}
	catch (Poco::Exception& e)
	{
		#ifdef TESTING
			std::cout << "extDB: Database Setup Failed: " << e.displayText() << std::endl;
		#endif
		BOOST_LOG_SEV(logger, boost::log::trivial::fatal) << "extDB: Database Setup Failed: " << e.displayText();
		db_conn_info = DBConnectionInfo();
		std::exit(EXIT_FAILURE);
	}
}


std::string Ext::version() const
{
	return "17";
}


std::string Ext::getAPIKey()
{
	return steam_api_key;
}


int Ext::getUniqueID_mutexlock()
{
	boost::lock_guard<boost::mutex> lock(mutex_unique_id);
	return mgr.get()->AllocateId();
}


void Ext::freeUniqueID_mutexlock(const int &unique_id)
{
	boost::lock_guard<boost::mutex> lock(mutex_unique_id);
	mgr.get()->FreeId(unique_id);
}

Poco::Data::Session Ext::getDBSession_mutexlock()
// Gets available DB Session (mutex lock)
{
	try
	{
		boost::lock_guard<boost::mutex> lock(mutex_db_pool);
		Poco::Data::Session free_session =  db_pool->get();
		return free_session;
	}
	catch (Poco::Data::SessionPoolExhaustedException&)
	//	Exceptiontal Handling in event of scenario if all asio worker threads are busy using all db connections
	//		And there is SYNC call using db & db_pool = exhausted
	{
		Poco::Data::Session new_session(db_conn_info.db_type, db_conn_info.connection_str);
		return new_session;
	}
}

std::string Ext::getDBType()
{
	return db_conn_info.db_type;
}

void Ext::getResult_mutexlock(const int &unique_id, char *output, const int &output_size)
// Gets Result String from unordered map array
//   If length of String = 0, sends arma "", and removes entry from unordered map array
//   If <=, then sends output to arma
//   If >, then sends 1 part to arma + stores rest.
{
	boost::lock_guard<boost::mutex> lock(mutex_unordered_map_results); // TODO Try to make Mutex Lock smaller
	boost::unordered_map<int, std::string>::const_iterator it = unordered_map_results.find(unique_id);
	if (it == unordered_map_results.end()) // NO UNIQUE ID or WAIT
	{
		if (unordered_map_wait.count(unique_id) == 0)
		{
			std::strcpy(output, (""));
		}
		else
		{
			std::strcpy(output, ("[3]"));
		}
	}
	else if (it->second.empty()) // END of MSG
	{
		unordered_map_results.erase(unique_id);
		freeUniqueID_mutexlock(unique_id);
		std::strcpy(output, (""));
	}
	else // SEND MSG (Part)
	{
		std::string msg = it->second.substr(0, output_size-1);
		std::strcpy(output, msg.c_str());
		if (it->second.length() > (output_size-1))
		{
			unordered_map_results[unique_id] = it->second.substr(output_size-1);
		}
		else
		{
			unordered_map_results[unique_id].clear();
		}
	}
}


void Ext::saveResult_mutexlock(const std::string &result, const int &unique_id)
// Stores Result String  in a unordered map array.
//   Used when string > arma output char
{
	boost::lock_guard<boost::mutex> lock(mutex_unordered_map_results);
	unordered_map_results[unique_id] = "[1," + result + "]";
	unordered_map_wait.erase(unique_id);
}


void Ext::addProtocol(char *output, const int &output_size, const std::string &protocol, const std::string &protocol_name, const std::string &init_data)
{
	{
		// TODO Implement Poco ClassLoader -- dayz hive ext has it to load database dll
		boost::lock_guard<boost::mutex> lock(mutex_unordered_map_protocol);
		if (boost::iequals(protocol, std::string("MISC")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new MISC());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("LOG")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new LOG());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_BASIC_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_BASIC_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_CUSTOM_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_CUSTOM_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_PROCEDURE_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_PROCEDURE_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_RAW_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_RAW_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else if (boost::iequals(protocol, std::string("DB_RAW_NO_EXTRA_QUOTES_V2")) == 1)
		{
			unordered_map_protocol[protocol_name] = boost::shared_ptr<AbstractProtocol> (new DB_RAW_NO_EXTRA_QUOTES_V2());
			if (!unordered_map_protocol[protocol_name].get()->init(this, init_data))
			// Remove Class Instance if Failed to Load
			{
				unordered_map_protocol.erase(protocol_name);
				std::strcpy(output, "[0,\"Failed to Load Protocol\"]");
			}
			else
			{
				std::strcpy(output, "[1]");
			}
		}
		else
		{
			std::strcpy(output, "[0,\"Error Unknown Protocol\"]");
		}
	}
}


void Ext::syncCallProtocol(char *output, const int &output_size, const std::string &protocol, const std::string &data)
// Sync callPlugin
{
	boost::unordered_map< std::string, boost::shared_ptr<AbstractProtocol> >::const_iterator itr = unordered_map_protocol.find(protocol);
	if (itr == unordered_map_protocol.end())
	{
		std::strcpy(output, ("[0,\"Error Unknown Protocol\"]"));
		BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Unknown Protocol: " + protocol);
	}
	else
	{
		// Checks if Result String will fit into arma output char
		//   If <=, then sends output to arma
		//   if >, then sends ID Message arma + stores rest. (mutex locks)
		std::string result;
		result.reserve(2000);
		itr->second->callProtocol(this, data, result);
		if (result.length() <= (output_size-9))
		{
			std::strcpy(output, ("[1, " + result + "]").c_str());
		}
		else
		{
			const int unique_id = getUniqueID_mutexlock();
			saveResult_mutexlock(result, unique_id);
			std::strcpy(output, ("[2,\"" + Poco::NumberFormatter::format(unique_id) + "\"]").c_str());
		}
	}
}


void Ext::onewayCallProtocol(const std::string protocol, const std::string data)
// ASync callProtocol
{
	boost::unordered_map< std::string, boost::shared_ptr<AbstractProtocol> >::const_iterator itr = unordered_map_protocol.find(protocol);
	if (itr != unordered_map_protocol.end())
	{
		std::string result;
		result.reserve(2000);
		itr->second->callProtocol(this, data, result);
	}
}


void Ext::asyncCallProtocol(const std::string protocol, const std::string data, const int unique_id)
// ASync + Save callProtocol
// We check if Protocol exists here, since its a thread (less time spent blocking arma) and it shouldnt happen anyways
{
	std::string result;
	result.reserve(2000);
	unordered_map_protocol[protocol].get()->callProtocol(this, data, result);
	saveResult_mutexlock(result, unique_id);
}


void Ext::callExtenion(char *output, const int &output_size, const char *function)
{
	try
	{
		#ifdef DEBUG_LOGGING
			BOOST_LOG_SEV(logger, boost::log::trivial::trace) << "Extension Input from Server: " +  std::string(function);
		#endif
		const std::string input_str(function);
		if (input_str.length() <= 2)
		{
			std::strcpy(output, ("[0,\"Error Invalid Message\"]"));
			BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Invalid Message: " + input_str);
		}
		else
		{
			const std::string sep_char(":");

			// Async / Sync
			const int async = Poco::NumberParser::parse(input_str.substr(0,1));

			switch (async)  // TODO Profile using Numberparser versus comparsion of char[0] + if statement checking length of *function
			{
				case 2: //ASYNC + SAVE
				{
					// Protocol
					const std::string::size_type found = input_str.find(sep_char,2);

					if (found==std::string::npos)  // Check Invalid Format
					{
						std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
						BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Invalid Format: " + input_str);
					}
					else
					{
						bool found_procotol = false;
						const std::string protocol = input_str.substr(2,(found-2));
						// Data
						std::string data = input_str.substr(found+1);
						int unique_id = getUniqueID_mutexlock();

						// Check for Protocol Name Exists...
						// Do this so if someone manages to get server, the error message wont get stored in the result unordered map
						{
							boost::lock_guard<boost::mutex> lock(mutex_unordered_map_results);
							if (unordered_map_protocol.find(protocol) == unordered_map_protocol.end())
							{
								std::strcpy(output, ("[0,\"Error Unknown Protocol\"]"));
								BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Unknown Protocol: " + protocol);
							}
							else
							{
								unordered_map_wait[unique_id] = true;
								found_procotol = true;
							}
						}
						// Only Add Job to Work Queue + Return ID if Protocol Name exists.
						if (found_procotol)
						{
							io_service.post(boost::bind(&Ext::asyncCallProtocol, this, protocol, data, unique_id));
							std::strcpy(output, (("[2,\"" + Poco::NumberFormatter::format(unique_id) + "\"]")).c_str());
						}
					}
					break;
				}
				case 5: // GET
				{
					const int unique_id = Poco::NumberParser::parse(input_str.substr(2));
					getResult_mutexlock(unique_id, output, output_size);
					break;
				}
				case 1: //ASYNC
				{
					// Protocol
					const std::string::size_type found = input_str.find(sep_char,2);

					if (found==std::string::npos)  // Check Invalid Format
					{
						std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
						BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Invalid Format: " + input_str);
					}
					else
					{
						const std::string protocol = input_str.substr(2,(found-2));
						// Data
						const std::string data = input_str.substr(found+1);
						io_service.post(boost::bind(&Ext::onewayCallProtocol, this, protocol, data));
						std::strcpy(output, "[1]");
					}
					break;
				}
				case 0: //SYNC
				{
					// Protocol
					const std::string::size_type found = input_str.find(sep_char,2);

					if (found==std::string::npos)  // Check Invalid Format
					{
						std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
						BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Invalid Format: " + input_str);
					}
					else
					{
						const std::string protocol = input_str.substr(2,(found-2));
						// Data
						const std::string data = input_str.substr(found+1);
						syncCallProtocol(output, output_size, protocol, data);
					}
					break;
				}
				case 9:
				{
					if (!extDB_lock)
					{
						// Protocol

						Poco::StringTokenizer tokens(input_str, ":");
						std::size_t token_count = tokens.count();
						
						switch (token_count)
						{
							case 2:
								// LOCK / VERSION
								if (tokens[1] == "VERSION")
								{
									std::strcpy(output, version().c_str());
								}
								else if (tokens[1] == "LOCK")
								{
									extDB_lock = true;
									std::strcpy(output, ("[1]"));
								}
								else if (tokens[1] == "OUTPUTSIZE")
									std::string outputsize_str = Poco::NumberFormatter::format(output_size);
									BOOST_LOG_SEV(logger, boost::log::trivial::info) << "Extension Output Size: " + outputsize_str;
									std::strcpy(output, outputsize_str);
								else
								{
									std::strcpy(output, ("[0,\"Error Invalid Format\"]"));	
									BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Invalid Format: " + input_str);
								}
								break;
							case 3:
								// DATABASE
								connectDatabase(output, output_size, tokens[2]);
								break;
							case 4:
								// ADD PROTOCOL
								addProtocol(output, output_size, tokens[2], tokens[3], "");
								break;
							case 5:
								//ADD PROTOCOL
								addProtocol(output, output_size, tokens[2], tokens[3], tokens[4]);
								break;
							default:
								// Invalid Format
								std::strcpy(output, ("[0,\"Error Invalid Format\"]"));
								BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Invalid Format: " + input_str);
						}
					}
					break;
				}
				default:
				{
					std::strcpy(output, ("[0,\"Error Invalid Message\"]"));
					BOOST_LOG_SEV(logger, boost::log::trivial::warning) << ("extDB: Invalid Message: " + input_str);
				}
			}
		}
	}
	catch (Poco::Exception& e)
	{
		std::strcpy(output, ("[0,\"Error Invalid Message\"]"));
		#ifdef TESTING
			std::cout << "extDB: Error: " << e.displayText() << std::endl;
		#endif
		BOOST_LOG_SEV(logger, boost::log::trivial::warning) << "extDB: Error: " + e.displayText();
	}
}


#ifdef TEST_APP
int main(int nNumberofArgs, char* pszArgs[])
{
	std::cout << std::endl << "Welcome to extDB Test Application : " << std::endl;
	std::cout << "    This application has 4096 char limited input." << std::endl;
	std::cout << "         Extension doesn't have this problem" << std::endl;
	std::cout << " To exit type 'quit'" << std::endl << std::endl;
	Ext *extension;
	extension = (new Ext());
	char result[4096];
	for (;;) {
		char input_str[4096];
		std::cin.getline(input_str, sizeof(input_str));
		if (std::string(input_str) == "quit")
		{
		    break;
		}
		else
		{
			extension->callExtenion(result, 80, input_str);
			std::cout << "extDB: " << result << std::endl;
		}
	}
	std::cout << "extDB Test: Quitting Please Wait" << std::endl;
	extension->stop();
	//delete extension;
	return 0;
}
#endif