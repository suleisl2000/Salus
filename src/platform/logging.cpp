/*
 * <one line to give the library's name and an idea of what it does.>
 * Copyright (C) 2017  Aetf <aetf@unlimitedcodeworks.xyz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "logging.h"

#include "crashhandler/crashhandler.hpp"
#include "utils/stringutils.h"

#include "executor.pb.h"

#include <zmq.hpp>

logging::LoggerWrapper::LoggerWrapper()
{
    spdlog::set_async_mode(8192);
    m_logger = spdlog::stdout_color_mt("console");

    m_logger->flush_on(spdlog::level::err);
    m_logger->set_level(spdlog::level::trace);

    g3::installCrashHandler();
}

logging::LoggerWrapper::~LoggerWrapper() = default;

std::shared_ptr<spdlog::logger> &logging::LoggerWrapper::logger()
{
    static LoggerWrapper wrapper;
    return wrapper.m_logger;
}

std::ostream &operator<<(std::ostream &os, const executor::OpKernelDef &c)
{
    return os << "OpKernelDef(" << c.id() << ", "
              << executor::OpKernelDef::OpLibraryType_Name(c.oplibrary()) << ")";
}

std::ostream &operator<<(std::ostream &os, const zmq::message_t &c)
{
    return os << "zmq::message_t(len=" << c.size()
              << ", data='" << utils::bytesToHexString(c.data<uint8_t>(), c.size()) << "')";
}

std::ostream &operator<<(std::ostream &os, const zmq::error_t &c)
{
    return os << "zmq::error_t(code=" << c.num()
              << ", msg='" << c.what() << "')";
}
