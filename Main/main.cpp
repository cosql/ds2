//
// Copyright (c) 2014, Facebook, Inc.
// All rights reserved.
//
// This source code is licensed under the University of Illinois/NCSA Open
// Source License found in the LICENSE file in the root directory of this
// source tree. An additional grant of patent rights can be found in the
// PATENTS file in the same directory.
//

#include "DebugServer2/BreakpointManager.h"
#include "DebugServer2/GDBRemote/DebugSessionImpl.h"
#include "DebugServer2/GDBRemote/PlatformSessionImpl.h"
#include "DebugServer2/GDBRemote/ProtocolHelpers.h"
#include "DebugServer2/GDBRemote/SlaveSessionImpl.h"
#include "DebugServer2/Host/Platform.h"
#include "DebugServer2/Host/QueueChannel.h"
#include "DebugServer2/Host/Socket.h"
#include "DebugServer2/Utils/Log.h"
#include "DebugServer2/Utils/OptParse.h"
#include "DebugServer2/Utils/String.h"

#include "SessionThread.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <set>
#include <string>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using ds2::Host::Socket;
using ds2::Host::QueueChannel;
using ds2::Host::Platform;
using ds2::BreakpointManager;
using ds2::GDBRemote::Session;
using ds2::GDBRemote::SessionDelegate;
using ds2::GDBRemote::PlatformSessionImpl;
using ds2::GDBRemote::DebugSessionImpl;
using ds2::GDBRemote::SlaveSessionImpl;

static uint16_t gDefaultPort = 12345;
static bool gKeepAlive = false;
static bool gLLDBCompat = false;

#if !defined(_WIN32)
static void PlatformMain(int argc, char **argv, int port) {
  Socket *server = new Socket;

  if (!server->create()) {
    DS2LOG(Main, Error, "cannot create server socket on port %d: %s", port,
           server->error().c_str());
    exit(EXIT_FAILURE);
  }

  if (!server->listen(port)) {
    DS2LOG(Main, Error, "error: failed to listen: %s", server->error().c_str());
    exit(EXIT_FAILURE);
  }

  DS2LOG(Main, Info, "listening on port %d", port);

  PlatformSessionImpl impl;

  do {
    Socket *client = server->accept();

    // Platform mode implies that we are talking to an LLDB remote.
    Session session(ds2::GDBRemote::kCompatibilityModeLLDB);
    session.setDelegate(&impl);
    session.create(client);

    while (session.receive(/*cooked=*/false))
      ;
  } while (gKeepAlive);

  exit(EXIT_SUCCESS);
}
#endif

static void RunDebugServer(Socket *server, SessionDelegate *impl) {
  Socket *client = server->accept();

  Session session(gLLDBCompat ? ds2::GDBRemote::kCompatibilityModeLLDB
                              : ds2::GDBRemote::kCompatibilityModeGDB);
  QueueChannel *qchannel = new QueueChannel(client);
  SessionThread thread(qchannel, &session);

  session.setDelegate(impl);
  session.create(qchannel);

  DS2LOG(Main, Debug, "DEBUG SERVER STARTED");

  thread.start();

  while (session.receive(/*cooked=*/true))
    ;

  DS2LOG(Main, Debug, "DEBUG SERVER KILLED");
}

static void DebugMain(ds2::StringCollection const &args,
                      ds2::EnvironmentBlock const &env, int attachPid,
                      int port) {
  Socket *server = new Socket;

  if (!server->create()) {
    DS2LOG(Main, Error, "cannot create server socket on port %d: %s", port,
           server->error().c_str());
    exit(EXIT_FAILURE);
  }

  if (!server->listen(port)) {
    DS2LOG(Main, Error, "failed to listen: %s", server->error().c_str());
    exit(EXIT_FAILURE);
  }

  DS2LOG(Main, Info, "listening on port %d", port);

  if (gLLDBCompat) {
    //
    // This is required for compatibility with llgs. The testing framework
    // expects to read this string to determine that llgs is started and ready
    // to accept connections.
    //
    printf("ds2\n");
    printf("Listening to port %d for a connection from %s...\n", port,
           "localhost");
  }

  do {
    DebugSessionImpl *impl;

    if (attachPid > 0)
      impl = new DebugSessionImpl(attachPid);
    else if (args.size() > 0)
      impl = new DebugSessionImpl(args, env);
    else
      impl = new DebugSessionImpl();

    RunDebugServer(server, impl);

    delete impl;
  } while (gKeepAlive);
}

#if !defined(_WIN32)
static void SlaveMain(int argc, char **argv) {
  Socket *server = new Socket;

  if (!server->create())
    exit(EXIT_FAILURE);

  if (!server->listen(0))
    exit(EXIT_FAILURE);

  uint16_t port = server->port();

  pid_t pid = ::fork();
  if (pid < 0)
    exit(EXIT_FAILURE);

  if (pid == 0) {
    //
    // Let the slave have its own session
    // TODO maybe should be a command line switch?
    //
    setsid();

    //
    // When in slave mode, output is suppressed but
    // for standard error.
    //
    close(0);
    close(1);

    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);

    RunDebugServer(server, new SlaveSessionImpl);
  } else {
    //
    // Write to the standard output to let know our parent
    // where we're listening.
    //
    fprintf(stdout, "%u %d\n", port, pid);

    DS2LOG(Main, Info, "listening on port %u pid %d", port, pid);
  }

  exit(EXIT_SUCCESS);
}
#endif

static void ListProcesses() {
  printf("Processes running on %s:\n\n", Platform::GetHostName());
  printf("%s\n%s\n", "PID    USER       ARCH    NAME",
         "====== ========== ======= ============================");

  Platform::EnumerateProcesses(true, ds2::UserId(),
                               [&](ds2::ProcessInfo const &info) {
    std::string pid = ds2::ToString(info.pid);

    std::string user;
    if (!Platform::GetUserName(info.realUid, user)) {
#if !defined(_WIN32)
      user = ds2::ToString(info.realUid);
#else
      user = "<NONE>";
#endif
    }

    std::string path = info.name;
    size_t lastsep =
#if defined(_WIN32)
        path.rfind('\\');
#else
        path.rfind('/');
#endif
    if (lastsep != std::string::npos) {
      path = path.substr(lastsep + 1);
    }

    printf("%-6s %-10.10s %-7.7s %s\n", pid.c_str(), user.c_str(),
           ds2::GetArchName(info.cpuType, info.cpuSubType), path.c_str());
  });

  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  ds2::OptParse opts;
  int idx;

  ds2::Host::Platform::Initialize();
  ds2::SetLogColorsEnabled(false);
#if !defined(_WIN32)
  ds2::SetLogColorsEnabled(isatty(fileno(stderr)));
#endif
  ds2::SetLogLevel(ds2::kLogLevelWarning);
  ds2::SetLogMask(~0U);

  enum RunMode {
    kRunModeNormal,
#if !defined(_WIN32)
    kRunModePlatform,
    kRunModeSlave,
#endif
  };

  int attachPid = -1;
  int port = -1;
  RunMode mode = kRunModeNormal;

  // Configuration options.
  opts.addOption(ds2::OptParse::stringOption, "log-output", 'o',
                 "output log message to the file specified");
  opts.addOption(ds2::OptParse::boolOption, "debug-remote", 'R',
                 "enable debugging of remote protocol");
  opts.addOption(ds2::OptParse::boolOption, "no-colors", 'n',
                 "disable colored output");
  opts.addOption(ds2::OptParse::boolOption, "keep-alive", 'k',
                 "keep the server alive after the client disconnects");

  // Target debug options.
  opts.addOption(ds2::OptParse::stringOption, "attach", 'a',
                 "attach to the name or PID specified");
  opts.addOption(ds2::OptParse::stringOption, "port", 'p',
                 "listen on the port specified");
  opts.addOption(ds2::OptParse::vectorOption, "set-env", 'e',
                 "add an element to the environment before launch");
  opts.addOption(ds2::OptParse::vectorOption, "unset-env", 'E',
                 "remove an element from the environment before lauch");

  // Non-debugserver options.
  opts.addOption(ds2::OptParse::boolOption, "list-processes", 'L',
                 "list processes debuggable by the current user");

#if !defined(_WIN32)
  // Platform mode.
  opts.addOption(ds2::OptParse::boolOption, "platform", 'P',
                 "execute in platform mode");
  opts.addOption(ds2::OptParse::boolOption, "slave", 'S',
                 "run in slave mode (used from platform spawner)");
#endif

  // llgs-compat options.
  opts.addOption(ds2::OptParse::boolOption, "lldb-compat", 'l',
                 "force ds2 to run in lldb compat mode");
  opts.addOption(ds2::OptParse::boolOption, "native-regs", 'r',
                 "use native registers (no-op)", true);
  opts.addOption(ds2::OptParse::boolOption, "setsid", 's',
                 "make ds2 run in its own session (no-op)", true);
  opts.addOption(ds2::OptParse::stringOption, "lldb-command", 'c',
                 "run lldb commands (no-op)", true);

  idx = opts.parse(argc, argv);

  if (!opts.getString("log-output").empty()) {
    FILE *stream = fopen(opts.getString("log-output").c_str(), "a");
    if (stream == nullptr) {
      DS2LOG(Main, Error, "unable to open %s for writing: %s",
             opts.getString("log-output").c_str(), strerror(errno));
    } else {
#if !defined(_WIN32)
      //
      // Note(sas): When ds2 is spawned by the app, it will run with the
      // app's user/group ID, and will create its log file owned by the
      // app. By default, the permissions will be 0600 (rw-------) which
      // makes us unable to get the log files. chmod() them to be able to
      // access them.
      //
      fchmod(fileno(stream), 0644);
#endif
      ds2::SetLogColorsEnabled(false);
      ds2::SetLogOutputStream(stream);
      ds2::SetLogLevel(ds2::kLogLevelDebug);
    }
  }

  if (opts.getBool("debug-remote")) {
    ds2::SetLogLevel(ds2::kLogLevelDebug);
  }

  if (opts.getBool("no-colors")) {
    ds2::SetLogColorsEnabled(false);
  }

  gKeepAlive = opts.getBool("keep-alive");

  if (!opts.getString("attach").empty()) {
    attachPid = atoi(opts.getString("attach").c_str());
  }

  if (!opts.getString("port").empty()) {
    port = atoi(opts.getString("port").c_str());
  }

  if (opts.getBool("list-processes")) {
    ListProcesses();
  }

#if !defined(_WIN32)
  if (opts.getBool("platform")) {
    mode = kRunModePlatform;
    //
    // The platform spawner should stay alive by default.
    //
    gKeepAlive = true;
  }

  if (opts.getBool("slave")) {
    mode = kRunModeSlave;
  }
#endif

  //
  // This option forces ds2 to operate in lldb compatibilty mode. When not
  // specified, we assume we are talking to a GDB remote until we detect
  // otherwise.
  //
  gLLDBCompat = opts.getBool("lldb-compat");

  argc -= idx, argv += idx;

  //
  // We need a process to attach to or a command line so we can launch it,
  // unless we are in lldb compat mode, which implies we can launch the debug
  // server without any of those two things, and wait for an "A" command that
  // specifies the command line to use to launch the inferior.
  //
  if (mode == kRunModeNormal && argc == 0 && attachPid < 0 && !gLLDBCompat) {
    opts.usageDie("either a program or target PID is required");
  }

  if (port < 0) {
    port = gDefaultPort;
  }

  switch (mode) {
#if !defined(_WIN32)
  case kRunModePlatform:
    PlatformMain(argc, argv, port);
    break;

  case kRunModeSlave:
    SlaveMain(argc, argv);
    break;
#endif

  case kRunModeNormal: {
    ds2::StringCollection args(&argv[0], &argv[argc]);
    ds2::EnvironmentBlock env;

    Platform::GetCurrentEnvironment(env);

    for (auto const &e : opts.getVector("set-env")) {
      char const *arg = e.c_str();
      char const *equal = strchr(arg, '=');
      if (equal == nullptr || equal == arg)
        DS2LOG(Main, Fatal, "invalid environment value: %s", arg);
      env[std::string(arg, equal)] = equal + 1;
    }

    for (auto const &e : opts.getVector("unset-env")) {
      env.erase(e);
    }

    DebugMain(args, env, attachPid, port);
  } break;
  }

  exit(EXIT_FAILURE);
  return -1;
}
