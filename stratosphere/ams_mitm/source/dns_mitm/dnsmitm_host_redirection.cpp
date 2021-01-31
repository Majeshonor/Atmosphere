/*
 * Copyright (c) 2018-2020 Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>
#include "../amsmitm_fs_utils.hpp"
#include "dnsmitm_debug.hpp"
#include "dnsmitm_host_redirection.hpp"
#include "socket_allocator.hpp"

namespace ams::mitm::socket::resolver {

    namespace {

        constexpr const char DefaultHostsFile[] =
            "# Nintendo telemetry servers\n"
            "127.0.0.1 receive-lp1.dg.srv.nintendo.net\n"
            "127.0.0.1 receive-lp1.er.srv.nintendo.net\n";

        constinit os::SdkMutex g_redirection_lock;
        std::unordered_map<std::string, ams::socket::InAddrT> g_redirection_map;

        constinit char g_specific_emummc_hosts_path[0x40] = {};

        void ParseHostsFile(const char *file_data) {
            enum class State {
                IgnoredLine,
                BeginLine,
                Ip1,
                IpDot1,
                Ip2,
                IpDot2,
                Ip3,
                IpDot3,
                Ip4,
                WhiteSpace,
                HostName,
            };

            ams::socket::InAddrT current_address;
            char current_hostname[0x200];
            u32 work;

            State state = State::BeginLine;
            for (const char *cur = file_data; *cur != '\x00'; ++cur) {
                const char c = *cur;
                switch (state) {
                    case State::IgnoredLine:
                        if (c == '\n') {
                            state = State::BeginLine;
                        }
                        break;
                    case State::BeginLine:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            current_address = 0;
                            work            = static_cast<u32>(c - '0');
                            state           = State::Ip1;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::Ip1:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            work *= 10;
                            work += static_cast<u32>(c - '0');
                        } else if (c == '.') {
                            current_address |= (work & 0xFF) << 0;
                            work = 0;
                            state = State::IpDot1;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::IpDot1:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            work            = static_cast<u32>(c - '0');
                            state           = State::Ip2;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::Ip2:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            work *= 10;
                            work += static_cast<u32>(c - '0');
                        } else if (c == '.') {
                            current_address |= (work & 0xFF) << 8;
                            work = 0;
                            state = State::IpDot2;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::IpDot2:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            work            = static_cast<u32>(c - '0');
                            state           = State::Ip3;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::Ip3:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            work *= 10;
                            work += static_cast<u32>(c - '0');
                        } else if (c == '.') {
                            current_address |= (work & 0xFF) << 16;
                            work = 0;
                            state = State::IpDot3;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::IpDot3:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            work            = static_cast<u32>(c - '0');
                            state           = State::Ip4;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::Ip4:
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            work *= 10;
                            work += static_cast<u32>(c - '0');
                        } else if (c == ' ' || c == '\t') {
                            current_address |= (work & 0xFF) << 24;
                            work = 0;
                            state = State::WhiteSpace;
                        } else {
                            state = State::IgnoredLine;
                        }
                        break;
                    case State::WhiteSpace:
                        if (c == '\n') {
                            state = State::BeginLine;
                        } else if (c != ' ' && c != '\r' && c != '\t') {
                            current_hostname[0] = c;
                            work = 1;
                            state = State::HostName;
                        }
                        break;
                    case State::HostName:
                        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
                            AMS_ABORT_UNLESS(work < sizeof(current_hostname));
                            current_hostname[work] = '\x00';

                            g_redirection_map[static_cast<const char *>(current_hostname)] = current_address;

                            if (c == '\n') {
                                state = State::BeginLine;
                            } else {
                                state = State::WhiteSpace;
                            }
                        } else {
                            AMS_ABORT_UNLESS(work < sizeof(current_hostname) - 1);
                            current_hostname[work++] = c;
                        }
                }
            }

            if (state == State::HostName) {
                AMS_ABORT_UNLESS(work < sizeof(current_hostname));
                current_hostname[work] = '\x00';

                g_redirection_map[static_cast<const char *>(current_hostname)] = current_address;
            }
        }

        void Log(::FsFile &f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
        void Log(::FsFile &f, const char *fmt, ...) {
            char log_buf[0x100];
            int len = 0;
            {
                std::va_list vl;
                va_start(vl, fmt);
                len = util::VSNPrintf(log_buf, sizeof(log_buf), fmt, vl);
                va_end(vl);
            }

            s64 ofs;
            R_ABORT_UNLESS(::fsFileGetSize(std::addressof(f), std::addressof(ofs)));
            R_ABORT_UNLESS(::fsFileWrite(std::addressof(f), ofs, log_buf, len, FsWriteOption_Flush));
        }

        const char *SelectHostsFile(::FsFile &log_file) {
            Log(log_file, "Selecting hosts file...\n");
            const bool is_emummc = emummc::IsActive();
            const u32 emummc_id  = emummc::GetActiveId();
            util::SNPrintf(g_specific_emummc_hosts_path, sizeof(g_specific_emummc_hosts_path), "/hosts/emummc_%04x", emummc_id);

            if (is_emummc) {
                if (mitm::fs::HasAtmosphereSdFile(g_specific_emummc_hosts_path)) {
                    return g_specific_emummc_hosts_path;
                }
                Log(log_file, "Skipping %s because it does not exist...\n", g_specific_emummc_hosts_path);

                if (mitm::fs::HasAtmosphereSdFile("/hosts/emummc")) {
                    return "/hosts/emummc";
                }
                Log(log_file, "Skipping %s because it does not exist...\n", "/hosts/emummc");
            } else {
                if (mitm::fs::HasAtmosphereSdFile("/hosts/sysmmc")) {
                    return "/hosts/sysmmc";
                }
                Log(log_file, "Skipping %s because it does not exist...\n", "/hosts/sysmmc");
            }

            return "/hosts/default";
        }

    }

    void InitializeResolverRedirections(bool add_defaults) {
        /* Acquire exclusive access to the map. */
        std::scoped_lock lk(g_redirection_lock);

        /* Clear the redirections map. */
        g_redirection_map.clear();

        /* Open log file. */
        ::FsFile log_file;
        mitm::fs::DeleteAtmosphereSdFile("/dns_mitm_startup.log");
        R_ABORT_UNLESS(mitm::fs::CreateAtmosphereSdFile("/dns_mitm_startup.log", 0, ams::fs::CreateOption_None));
        R_ABORT_UNLESS(mitm::fs::OpenAtmosphereSdFile(std::addressof(log_file), "/dns_mitm_startup.log", ams::fs::OpenMode_ReadWrite | ams::fs::OpenMode_AllowAppend));
        ON_SCOPE_EXIT { ::fsFileClose(std::addressof(log_file)); };

        Log(log_file, "DNS Mitm:\n");

        /* If a default hosts file doesn't exist on the sd card, create one. */
        if (!mitm::fs::HasAtmosphereSdFile("/hosts/default")) {
            Log(log_file, "Creating /hosts/default because it does not exist.\n");

            mitm::fs::CreateAtmosphereSdDirectory("/hosts");
            R_ABORT_UNLESS(mitm::fs::CreateAtmosphereSdFile("/hosts/default", sizeof(DefaultHostsFile) - 1, ams::fs::CreateOption_None));

            ::FsFile default_file;
            R_ABORT_UNLESS(mitm::fs::OpenAtmosphereSdFile(std::addressof(default_file), "/hosts/default", ams::fs::OpenMode_ReadWrite));
            R_ABORT_UNLESS(::fsFileWrite(std::addressof(default_file), 0, DefaultHostsFile, sizeof(DefaultHostsFile) - 1, ::FsWriteOption_Flush));
            ::fsFileClose(std::addressof(default_file));
        }

        /* If we should, add the defaults. */
        if (add_defaults) {
            Log(log_file, "Adding defaults to redirection list.\n");
            ParseHostsFile(DefaultHostsFile);
        }

        /* Select the hosts file. */
        const char *hosts_path = SelectHostsFile(log_file);
        Log(log_file, "Selected %s\n", hosts_path);

        /* Load the hosts file. */
        {
            char *hosts_file_data = nullptr;
            ON_SCOPE_EXIT { if (hosts_file_data != nullptr) { ams::Free(hosts_file_data); } };
            {
                ::FsFile hosts_file;
                R_ABORT_UNLESS(mitm::fs::OpenAtmosphereSdFile(std::addressof(hosts_file), hosts_path, ams::fs::OpenMode_Read));
                ON_SCOPE_EXIT { ::fsFileClose(std::addressof(hosts_file)); };

                /* Get the hosts file size. */
                s64 hosts_size;
                R_ABORT_UNLESS(::fsFileGetSize(std::addressof(hosts_file), std::addressof(hosts_size)));

                /* Validate we can read the file. */
                AMS_ABORT_UNLESS(0 <= hosts_size && hosts_size < 0x8000);

                /* Read the data. */
                hosts_file_data = static_cast<char *>(ams::Malloc(0x8000));
                AMS_ABORT_UNLESS(hosts_file_data != nullptr);

                u64 br;
                R_ABORT_UNLESS(::fsFileRead(std::addressof(hosts_file), 0, hosts_file_data, hosts_size, ::FsReadOption_None, std::addressof(br)));
                AMS_ABORT_UNLESS(br == static_cast<u64>(hosts_size));

                /* Null-terminate. */
                hosts_file_data[hosts_size] = '\x00';
            }

            /* Parse the hosts file. */
            ParseHostsFile(hosts_file_data);
        }

        /* Print the redirections. */
        Log(log_file, "Redirections:\n");
        for (const auto &[host, address] : g_redirection_map) {
            Log(log_file, "    `%s` -> %u.%u.%u.%u\n", host.c_str(), (address >> 0) & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF, (address >> 24) & 0xFF);
        }
    }

    bool GetRedirectedHostByName(ams::socket::InAddrT *out, const char *hostname) {
        std::scoped_lock lk(g_redirection_lock);

        for (const auto &[host, address] : g_redirection_map) {
            if (std::strcmp(host.c_str(), hostname) == 0) {
                *out = address;
                return true;
            }
        }

        return false;
    }

}
