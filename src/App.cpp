/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2016-2017 XMRig       <support@xmrig.com>
 * Copyright 2017-     BenDr0id    <ben@graef.in>
 *
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdlib.h>
#include <uv.h>
#include <cc/ControlCommand.h>

#include "App.h"
#include "Console.h"
#include "Cpu.h"
#include "crypto/CryptoNight.h"
#include "log/ConsoleLog.h"
#include "log/FileLog.h"
#include "log/RemoteLog.h"
#include "log/Log.h"
#include "Mem.h"
#include "net/Network.h"
#include "Platform.h"
#include "Summary.h"
#include "workers/Workers.h"
#include "cc/CCClient.h"
#include "net/Url.h"


#ifdef HAVE_SYSLOG_H
#   include "log/SysLog.h"
#endif

#ifndef XMRIG_NO_HTTPD
#   include "api/Httpd.h"
#   include "api/Api.h"
#endif


App *App::m_self = nullptr;


App::App(int argc, char **argv) :
    m_restart(false),
    m_console(nullptr),
    m_httpd(nullptr),
    m_network(nullptr),
    m_options(nullptr),
    m_ccclient(nullptr)
{
    m_self = this;

    Cpu::init();

    m_options = Options::parse(argc, argv);
    if (!m_options) {
        return;
    }

    Log::init();

#   ifdef WIN32
    if (!m_options->background()) {
#   endif
        Log::add(new ConsoleLog(m_options->colors()));
        m_console = new Console(this);
#   ifdef WIN32
    }
#   endif

    if (m_options->logFile()) {
        Log::add(new FileLog(m_options->logFile()));
    }

    if (m_options->ccUseRemoteLogging()) {
        // 20 lines per second should be enough
        Log::add(new RemoteLog(static_cast<size_t>(m_options->ccUpdateInterval() * 20)));
    }

#   ifdef HAVE_SYSLOG_H
    if (m_options->syslog()) {
        Log::add(new SysLog());
    }
#   endif

    Platform::init(m_options->userAgent());
    Platform::setProcessPriority(m_options->priority());

    m_network = new Network(m_options);

    uv_signal_init(uv_default_loop(), &m_sigHUP);
    uv_signal_init(uv_default_loop(), &m_sigINT);
    uv_signal_init(uv_default_loop(), &m_sigTERM);
}

App::~App()
{
    delete m_network;

    Options::release();
    Platform::release();

    uv_tty_reset_mode();

#   ifndef XMRIG_NO_HTTPD
    delete m_httpd;
#   endif

#   ifndef XMRIG_NO_CC
    if (m_ccclient) {
        delete m_ccclient;
    }
#   endif
}


int App::start()
{
    if (!m_options) {
        return EINVAL;
    }

    uv_signal_start(&m_sigHUP,  App::onSignal, SIGHUP);
    uv_signal_start(&m_sigINT,  App::onSignal, SIGINT);
    uv_signal_start(&m_sigTERM, App::onSignal, SIGTERM);

    background();

    if (Options::i()->colors()) {
        LOG_INFO(WHITE_BOLD("%s hash self-test"), m_options->algoName());
    }
    else {
        LOG_INFO("%s hash self-test", m_options->algoName());
    }

    if (!CryptoNight::init(m_options->algo(), m_options->aesni())) {
        LOG_ERR("%s hash self-test... failed.", m_options->algoName());
        return EINVAL;
    } else {
        if (Options::i()->colors()) {
            LOG_INFO(WHITE_BOLD("%s hash self-test... %s."),
                m_options->algoName(),
                Options::i()->skipSelfCheck() ?  YELLOW_BOLD("skipped") : GREEN_BOLD("successful"));
        }
        else {
            LOG_INFO("%s hash self-test... %s.",
                m_options->algoName(),
                Options::i()->skipSelfCheck() ?  "skipped" : "successful");
        }
    }

    Mem::init(m_options);

    Summary::print();

#   ifndef XMRIG_NO_API
    Api::start();
#   endif

#   ifndef XMRIG_NO_HTTPD
    m_httpd = new Httpd(m_options->apiPort(), m_options->apiToken());
    m_httpd->start();
#   endif

#   ifndef XMRIG_NO_CC
    if (m_options->ccHost() && m_options->ccPort() > 0) {
        uv_async_init(uv_default_loop(), &m_async, App::onCommandReceived);

        m_ccclient = new CCClient(m_options, &m_async);

        if (! m_options->pools().front()->isValid()) {
            LOG_WARN("No pool URL supplied, but CC server configured. Trying.");
        }
    } else {
        LOG_WARN("Please configure CC-Url and restart. CC feature is now deactivated.");
    }
#   endif

    Workers::start(m_options->threads(), m_options->affinity(), m_options->priority());

    if (m_options->pools().front()->isValid()) {
        m_network->connect();
    }

    const int r = uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    uv_loop_close(uv_default_loop());

    return m_restart ? EINTR : r;
}

void App::onConsoleCommand(char command)
{
    switch (command) {
    case 'h':
    case 'H':
        Workers::printHashrate(true);
        break;

    case 'p':
    case 'P':
        if (Workers::isEnabled()) {
            LOG_INFO(m_options->colors() ? "\x1B[01;33mpaused\x1B[0m, press \x1B[01;35mr\x1B[0m to resume" : "paused, press 'r' to resume");
            Workers::setEnabled(false);
        }
        break;

    case 'r':
    case 'R':
        if (!Workers::isEnabled()) {
            LOG_INFO(m_options->colors() ? "\x1B[01;32mresumed" : "resumed");
            Workers::setEnabled(true);
        }
        break;

    case 'q':
    case 'Q':
    case 3:
        LOG_INFO(m_options->colors() ? "\x1B[01;33mquitting" : "quitting");
        shutdown();
        break;

    default:
        break;
    }
}


void App::stop(bool restart)
{
    m_restart = restart;

    m_network->stop();
    Workers::stop();

    uv_stop(uv_default_loop());
}

void App::restart()
{
    m_self->stop(true);
}

void App::shutdown()
{
    m_self->stop(false);
}

void App::reboot()
{
    auto rebootCmd = m_self->m_options->ccRebootCmd();
    if (rebootCmd) {
        system(rebootCmd);
        shutdown();
    }
}

void App::onSignal(uv_signal_t* handle, int signum)
{
    switch (signum)
    {
    case SIGHUP:
        LOG_WARN("SIGHUP received, exiting");
        break;

    case SIGTERM:
        LOG_WARN("SIGTERM received, exiting");
        break;

    case SIGINT:
        LOG_WARN("SIGINT received, exiting");
        break;

    default:
        break;
    }

    uv_signal_stop(handle);
    App::shutdown();
}

void App::onCommandReceived(uv_async_t* async)
{
    auto command = reinterpret_cast<ControlCommand::Command &> (async->data);
    switch (command) {
        case ControlCommand::START:
            Workers::setEnabled(true);
            break;
        case ControlCommand::STOP:
            Workers::setEnabled(false);
            break;
        case ControlCommand::UPDATE_CONFIG:;
        case ControlCommand::RESTART:
            App::restart();
            break;
        case ControlCommand::SHUTDOWN:
            App::shutdown();
            break;
        case ControlCommand::REBOOT:
            App::reboot();
            break;
        case ControlCommand::PUBLISH_CONFIG:;
            break;
    }
}
