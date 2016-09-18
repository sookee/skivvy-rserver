/*
 * ircbot-rserver.cpp
 *
 *  Created on: 10 Dec 2011
 *      Author: oaskivvy@gmail.com
 */

/*-----------------------------------------------------------------.
| Copyright (C) 2011 SooKee oaskivvy@gmail.com               |
'------------------------------------------------------------------'

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.

http://www.gnu.org/licenses/gpl-2.0.html

'-----------------------------------------------------------------*/

#include <skivvy/plugin-rserver.h>

#include <ctime>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>

#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sookee/str.h>
#include <sookee/bug.h>
#include <sookee/log.h>
#include <skivvy/logrep.h>
#include <skivvy/utils.h>
#include <sookee/types.h>
#include <skivvy/socketstream.h>
#include <skivvy/irc-constants.h>

namespace skivvy { namespace ircbot {

IRC_BOT_PLUGIN(RServerIrcBotPlugin);
PLUGIN_INFO("rserver", "Remote Server", "0.1");

using namespace sookee::types;
using namespace skivvy::utils;
using namespace sookee::utils;

const str PORT_KEY = "rserver.port";
const str PORT_VAL = "7334";
const str BIND_KEY = "rserver.bind";
const str BIND_VAL = "0.0.0.0";

const auto BIND_RETRIES_KEY = "rserver.bind.retries";
const auto BIND_RETRIES_VAL = 10U;
const auto BIND_RETRY_PAUSE_KEY = "rserver.bind.retry.pause.ms";
const auto BIND_RETRY_PAUSE_VAL = 10000U;

const auto ACCEPT_IPV4_KEY = "rserver.accept.ipv4";
const auto ACCEPT_IPV6_KEY = "rserver.accept.ipv6";

RServerIrcBotPlugin::RServerIrcBotPlugin(IrcBot& bot)
: BasicIrcBotPlugin(bot), ss(::socket(PF_INET, SOCK_STREAM, 0))
, done(false)
, dusted(false)
{
//	fcntl(ss, F_SETFL, fcntl(ss, F_GETFL, 0) | O_NONBLOCK);
//	int option = 1;
//	if(setsockopt(ss, SOL_SOCKET, SO_REUSEPORT | SO_REUSEADDR, &option, sizeof(option)))
//		log("ERROR: " << std::strerror(errno));
}

//RServerIrcBotPlugin::~RServerIrcBotPlugin() {}

//bool RServerIrcBotPlugin::bind()
//{
//	bug_fun();
//	port p = bot.get(RSERVER_PORT, RSERVER_PORT_DEFAULT);
//	str host = bot.get(RSERVER_BIND, RSERVER_BIND_DEFAULT);
//	sockaddr_in addr;
//	std::memset(&addr, 0, sizeof(addr));
//	addr.sin_family = AF_INET;
//	addr.sin_port = htons(p);
//	addr.sin_addr.s_addr = inet_addr(host.c_str());
//	return ::bind(ss, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != -1;
//}

struct gai_scoper
{
	struct addrinfo* res;
	gai_scoper(struct addrinfo* res): res(res) {}
	~gai_scoper() { freeaddrinfo(res); }
};

bool RServerIrcBotPlugin::bind()
{
//	bug_fun();
	str port = bot.get(PORT_KEY, PORT_VAL);
	str host = bot.get(BIND_KEY, BIND_VAL);

	ufd.fd = ss; // setup polling
	ufd.events = POLLIN;

	addrinfo hints;
	addrinfo* res;

	// first, load up address structs with getaddrinfo():

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
	hints.ai_socktype = SOCK_STREAM;

	int error;
	if((error = getaddrinfo(host.c_str(), port.c_str(), &hints, &res)))
	{
		log("ERROR: " << gai_strerror(error));
		return false;
	}

	gai_scoper scoper(res);

	unsigned retry_pause_millis = bot.get(BIND_RETRY_PAUSE_KEY, BIND_RETRY_PAUSE_VAL);

//	bug_var(retry_pause_millis);

	if(retry_pause_millis < 500)
		retry_pause_millis = 500;

	if(retry_pause_millis > 60000)
		retry_pause_millis = 60000;

//	bug_var(retry_pause_millis);

	unsigned retries = bot.get(BIND_RETRIES_KEY, BIND_RETRIES_VAL);

//	bug_var(retries);

	if((retries * retry_pause_millis) / 1000 > 60 * 10) // max 10 minutes
		retries = 60 * 10 * 1000 / retry_pause_millis; // 10 minutes

//	bug_var(retries);

	hr_duration retry_pause = std::chrono::milliseconds(retry_pause_millis);

	while(::bind(ss, res->ai_addr, res->ai_addrlen) == -1)
	{
		if(errno == EADDRINUSE && retries)
		{
			log("ADDRESS IN USE: retrying in: " << retry_pause_millis << "ms [" << retries << "]");
			--retries;
			hr_time_point wait = hr_clk::now() + retry_pause;
			while(hr_clk::now() < wait)
				std::this_thread::sleep_until(wait);
			continue;
		}
		log("ERROR: " << std::strerror(errno));
		return false;
	}

	int option = 1;
	if(setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)))
	{
		log("ERROR: " << std::strerror(errno));
		return false;
	}

	return true;
}

bool RServerIrcBotPlugin::listen()
{
//	bug_fun();
	if(!bind())
	{
		log("ERROR: " << std::strerror(errno));
		return false;
	}

	if(::listen(ss, 10) == -1)
	{
		log("ERROR: " << std::strerror(errno));
		return false;
	}

	int rv;

	while(!this->done)
	{
		if(!(rv = poll(&ufd, 1, 1000)))
			continue;
		else if(rv == -1)
		{
			log("ERROR: " << std::strerror(errno));
			this->done = true;
			continue;
		}
		else if(rv > 0)
		{
//			bug_var(rv);
//			bug_var(ss);
			if(ufd.revents & POLLIN)
			{
				if(this->done)
					continue;

				sockaddr connectee;
				socklen_t connectee_len = sizeof(sockaddr);
				socket cs;

				if((cs = ::accept(ss, &connectee, &connectee_len)) == -1)
				{
					log("ERROR: " << std::strerror(errno));
					continue;
				}
				else if(!this->done)
				{
					if(connectee.sa_family == AF_INET6)
					{
						if(ipv6accepts.empty() || ipv6accepts.count(((sockaddr_in6*) &connectee)->sin6_addr))
							std::async(std::launch::async, [&]{ process(cs); });
						else
						{
							::close(cs);
							in6_addr in = ((sockaddr_in6*) &connectee)->sin6_addr;
							char buf[INET6_ADDRSTRLEN];
							str ip = inet_ntop(AF_INET6, &in, buf, INET6_ADDRSTRLEN);
							log("SECURITY: rejecting unauthorized connection attempt: " << ip);
						}
					}
					else if(connectee.sa_family == AF_INET)
					{
						if(ipv4accepts.empty() || ipv4accepts.count(((sockaddr_in*) &connectee)->sin_addr.s_addr))
							std::async(std::launch::async, [&]{ process(cs); });
						else
						{
							::close(cs);
							in_addr_t in = ((sockaddr_in*) &connectee)->sin_addr.s_addr;
							char buf[INET_ADDRSTRLEN];
							str ip = inet_ntop(AF_INET, &in, buf, INET_ADDRSTRLEN);
							log("SECURITY: rejecting unauthorized connection attempt: " << ip);
						}
					}
					else
					{
						log("WARN: only accepting connections for ipv4 & ipv6");
						::close(cs);
						continue;
					}
				}
			}
		}
	}
	::close(ss);
	dusted = true;
	return true;
}

void RServerIrcBotPlugin::process(socket cs)
{
	lock_guard lock(mtx);
	net::socketstream ss(cs);
	// receive null terminated string
	str cmd;

//	char c;
//	while(ss.get(c) && c)
//		cmd.append(1, c);
	sgl(ss, cmd, '\0');

//	bug("cmd: " << cmd);

	if(trim(cmd).empty())
		return;

	if(cmd == "/get_status")
	{
		for(const auto& s: status)
			ss << s << '\n';
		ss << std::flush;
		status.clear();
	}
	else
	{
		soss oss;
		bot.exec(cmd, &oss);
		ss << oss.str() << '\0' << std::flush;
	}
}

void RServerIrcBotPlugin::on(const message& msg)
{
	(void) msg;
//	BUG_COMMAND(msg);
}

void RServerIrcBotPlugin::off(const message& msg)
{
	(void) msg;
//	BUG_COMMAND(msg);
}

bool RServerIrcBotPlugin::initialize()
{
	bug_fun();

	{
		str_vec ips = bot.get_vec(ACCEPT_IPV4_KEY);

		in_addr_t in = {};

		for(const str& ip: ips)
		{
			int e = inet_pton(AF_INET, ip.c_str(), &in);

			if(e == -1)
				log("ERROR: " << strerror(errno));
			else if(e == 0)
				log("ERROR: ipv4 address not valid: " << ip);
			else if(e == 1)
			{
				ipv4accepts.insert(in);
				log("ADDING ALLOWED IPv4: " << ip);
			}
		}
	}

	{
		str_vec ips = bot.get_vec(ACCEPT_IPV6_KEY);

		in6_addr in = {};

		for(const str& ip: ips)
		{
			int e = inet_pton(AF_INET6, ip.c_str(), &in);

			if(e == -1)
				log("ERROR: " << strerror(errno));
			else if(e == 0)
				log("ERROR: ipv6 address not valid: " << ip);
			else if(e == 1)
			{
				ipv6accepts.insert(in);
				log("ADDING ALLOWED IPv6: " << ip);
			}
		}
	}

	log("RSERVER: " << ((ipv4accepts.empty() && ipv6accepts.empty())?"INSECURE":"SECURE") << " MODE");

	con = std::async(std::launch::async, [&]{ listen(); });

	bot.add_monitor(*this);
	return true;
}

// INTERFACE: IrcBotMonitor

static const str_vec event_commands
{
	  irc::NOTICE
	, irc::MODE
};

void RServerIrcBotPlugin::event(const message& msg)
{
//	bug_fun();
	if(std::find(event_commands.begin(), event_commands.end(), msg.command) == event_commands.end())
		return;

//	BUG_COMMAND(msg);
	if(msg.command == irc::NOTICE)
	{
		lock_guard lock(mtx);
		add_status_msg(msg.get_trailing());
	}
	if(msg.command == irc::MODE)
	{
		auto params = msg.get_params();
		if(params.size() == 3 && params[2] == bot.nick)
		{
			//---------------------------------------------------
			//                  line: :Galik!~galik@unaffiliated/galik MODE #skivvy +o Skivvy
			//                prefix: Galik!~galik@unaffiliated/galik
			//               command: MODE
			//                params:  #skivvy +o Skivvy
			// get_servername()     :
			// get_nickname()       : Galik
			// get_user()           : ~galik
			// get_host()           : unaffiliated/galik
			// param                : #skivvy
			// param                : +o
			// param                : Skivvy
			// middle               : #skivvy
			// middle               : +o
			// middle               : Skivvy
			// trailing             :
			// get_nick()           : Galik
			// get_chan()           : #skivvy
			// get_user_cmd()       :
			// get_user_params()    :
			//---------------------------------------------------
			if(params[1] == "+o")
			{
				lock_guard lock(mtx);
				add_status_msg(msg.get_nickname() + " gives channel operator status to "
					+ bot.nick);
			}
		}
	}
}

// INTERFACE: IrcBotPlugin

str RServerIrcBotPlugin::get_id() const { return ID; }
std::string RServerIrcBotPlugin::get_name() const { return NAME; }
std::string RServerIrcBotPlugin::get_version() const { return VERSION; }

void RServerIrcBotPlugin::exit()
{
	this->done = true;

	st_time_point timeout = st_clk::now() + std::chrono::seconds(30);

	while(!dusted && st_clk::now() < timeout)
		std::this_thread::sleep_for(std::chrono::seconds(1));

	if(!dusted)
		log("ERROR: server failed to stop");

	if(con.valid())
		con.get();
}

}} // sookee::ircbot
