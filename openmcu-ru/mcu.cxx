
#include <ptlib.h>

#include "version.h"
#include "mcu.h"
#include "h323.h"
#include "html.h"

#define new PNEW

///////////////////////////////////////////////////////////////
// This really isn't the default count only a counter
// for sending aliases and prefixes to the gatekeeper
int OpenMCU::defaultRoomCount = 5;

VideoMixConfigurator OpenMCU::vmcfg;

OpenMCU::OpenMCU()
  : OpenMCUProcessAncestor(ProductInfo)
{
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN); // PTCPSocket caused SIGPIPE on browser disconnect time to time
#endif
  endpoint          = NULL;
  sipendpoint       = NULL;
  currentLogLevel   = -1;
  currentTraceLevel = -1;
  traceFileRotated  = FALSE;
}

void OpenMCU::Main()
{
  Suspend();
}

BOOL OpenMCU::OnStart()
{
#if PTLIB_MAJOR == 2 && PTLIB_MINOR == 0
  char ** argv=PXGetArgv();
  executableFile = argv[0];
  PDirectory exeDir = executableFile.GetDirectory();
  exeDir.Change();
#endif

  SetConfigurationPath(CONFIG_PATH);

  httpNameSpace.AddResource(new PHTTPDirectory("data", "data"));
  httpNameSpace.AddResource(new PServiceHTTPDirectory("html", "html"));

  manager  = CreateConferenceManager();
  endpoint = CreateEndPoint(*manager);
  sipendpoint = new OpenMCUSipEndPoint(endpoint);

  return PHTTPServiceProcess::OnStart();
}

void OpenMCU::OnStop()
{
#if !(PTLIB_MAJOR == 2 && PTLIB_MINOR == 10)
  PHTTPServiceProcess::OnStop();
#endif

  sipendpoint->terminating = 1;
  sipendpoint->WaitForTermination(10000);
  delete sipendpoint;
  sipendpoint = NULL;

  delete endpoint;
  endpoint = NULL;

  delete manager;
  manager = NULL;

#ifndef _WIN32
  CommonDestruct(); // delete configFiles;
#endif
}

void OpenMCU::OnControl()
{
  // This function get called when the Control menu item is selected in the
  // tray icon mode of the service.
  PStringStream url;
  url << "http://";

  PString host = PIPSocket::GetHostName();
  PIPSocket::Address addr;
  if (PIPSocket::GetHostAddress(host, addr))
    url << host;
  else
    url << "localhost";

  url << ':' << DefaultHTTPPort;

  PURL::OpenBrowser(url);
}

BOOL OpenMCU::Initialise(const char * initMsg)
{
  PDirectory exeDir = executableFile.GetDirectory();
  exeDir.Change();

#ifdef _WIN32
  if(PXGetArgc()>1)
  {
    char ** argv=PXGetArgv();
    PString arg1=argv[1];
    if(arg1 *= "debug") cout.rdbuf(PError.rdbuf());
  }
#endif

  MCUConfig cfg("Parameters");

  vmcfg.go(vmcfg.bfw,vmcfg.bfh);

#if PTRACING
  int TraceLevel=cfg.GetInteger(TraceLevelKey, DEFAULT_TRACE_LEVEL);
  int LogLevel=cfg.GetInteger(LogLevelKey, DEFAULT_LOG_LEVEL);
  if(currentLogLevel != LogLevel)
  {
    SetLogLevel((PSystemLog::Level)LogLevel);
    currentLogLevel = LogLevel;
  }
  PINDEX rotationLevel = cfg.GetInteger(TraceRotateKey, 0);
  if(currentTraceLevel != TraceLevel)
  {
    if(!traceFileRotated)
    {
      if(rotationLevel != 0)
      {
        PString
          pfx = PString(SERVER_LOGS) + PATH_SEPARATOR + "trace",
          sfx = ".txt";
        PFile::Remove(pfx + PString(rotationLevel-1) + sfx, TRUE);
        for (PINDEX i=rotationLevel-1; i>0; i--) PFile::Move(pfx + PString(i-1) + sfx, pfx + PString(i) + sfx, TRUE);
        PFile::Move(pfx + sfx, pfx + "0" + sfx, TRUE);
      }
      traceFileRotated = TRUE;
    }

    PTrace::Initialise(TraceLevel, PString(SERVER_LOGS) + PATH_SEPARATOR + "trace.txt");
    PTrace::SetOptions(PTrace::FileAndLine);
    currentTraceLevel = TraceLevel;
  }

#  ifdef GIT_REVISION
#    define _QUOTE_MACRO_VALUE1(x) #x
#    define _QUOTE_MACRO_VALUE(x) _QUOTE_MACRO_VALUE1(x)
  PTRACE(1,"OpenMCU-ru git revision " << _QUOTE_MACRO_VALUE(GIT_REVISION));
#    undef _QUOTE_MACRO_VALUE
#    undef _QUOTE_MACRO_VALUE1
#  endif
#endif //if PTRACING

#ifdef __VERSION__
  PTRACE(1,"OpenMCU-ru GCC version " <<__VERSION__);
#endif

// default log file name
#ifdef SERVER_LOGS
  {
    PString lfn = cfg.GetString(CallLogFilenameKey, DefaultCallLogFilename);
    if(lfn.Find(PATH_SEPARATOR) == P_MAX_INDEX) logFilename = PString(SERVER_LOGS)+PATH_SEPARATOR+lfn;
    else logFilename = lfn;
  }
#else
  logFilename = cfg.GetString(CallLogFilenameKey, DefaultCallLogFilename);
#endif
  copyWebLogToLog = cfg.GetBoolean("Copy web log to call log", FALSE);
  // Buffered events
  httpBuffer=cfg.GetInteger(HttpLinkEventBufferKey, 100);
  httpBufferedEvents.SetSize(httpBuffer);
  httpBufferIndex=0; httpBufferComplete=0;

#if OPENMCU_VIDEO
  endpoint->enableVideo = cfg.GetBoolean("Enable video", TRUE);
  endpoint->videoRate = MCUConfig("Video").GetInteger("Video frame rate", DefaultVideoFrameRate);
  endpoint->videoTxQuality = cfg.GetInteger("Video quality", DefaultVideoQuality);
  forceScreenSplit = cfg.GetBoolean(ForceSplitVideoKey, TRUE);
#if USE_LIBYUV
  scaleFilter=libyuv::LIBYUV_FILTER;
#endif
#endif

  // recall last template after room created
  recallRoomTemplate = cfg.GetBoolean(RecallLastTemplateKey, FALSE);

  // get conference time limit
  roomTimeLimit = cfg.GetInteger(DefaultRoomTimeLimitKey, 0);

  // allow/disallow self-invite:
  allowLoopbackCalls = cfg.GetBoolean(AllowLoopbackCallsKey, FALSE);

#if P_SSL
  // Secure HTTP
  BOOL enableSSL = cfg.GetBoolean(HTTPSecureKey, FALSE);
  disableSSL = !enableSSL;
  if(enableSSL)
  {
    PString certificateFile = cfg.GetString(HTTPCertificateFileKey, DefaultHTTPCertificateFile);
    if (!SetServerCertificate(certificateFile, TRUE)) {
      PSYSTEMLOG(Fatal, "MCU\tCould not load certificate \"" << certificateFile << '"');
      return FALSE;
    }
  }
#endif

  // Get the HTTP authentication info
  MCUConfig("Managing Groups").SetString("administrator", "");
  MCUConfig("Managing Groups").SetString("conference manager", "");

  PHTTPMultiSimpAuth authSettings(GetName());
  PHTTPMultiSimpAuth authConference(GetName());
  PStringList keysUsers = MCUConfig("Managing Users").GetKeys();
  for(PINDEX i = 0; i < keysUsers.GetSize(); i++)
  {
    PStringArray params = MCUConfig("Managing Users").GetString(keysUsers[i]).Tokenise(",");
    if(params.GetSize() < 2) continue;
    if(params[1] == "administrator")
    {
      authSettings.AddUser(keysUsers[i], PHTTPPasswordField::Decrypt(params[0]));
      authConference.AddUser(keysUsers[i], PHTTPPasswordField::Decrypt(params[0]));
    }
    if(params[1] == "conference manager")
    {
      authConference.AddUser(keysUsers[i], PHTTPPasswordField::Decrypt(params[0]));
    }
  }

  PString dp = cfg.GetString(DefaultProtocolKey, "H.323");
  if(dp=="SIP") defaultProtocol=DEFAULT_SIP;
  else defaultProtocol=DEFAULT_H323;

  // get default "room" (conference) name
  defaultRoomName = cfg.GetString(DefaultRoomKey, DefaultRoom);
  lockTplByDefault = cfg.GetBoolean(LockTplByDefaultKey, FALSE);

  autoStartRecord = cfg.GetInteger(AutoStartRecorderKey, -1);
  autoStopRecord = cfg.GetInteger(AutoStopRecorderKey, -1);
  autoDeleteRoom = cfg.GetBoolean(AutoDeleteRoomKey, FALSE);

  { // video recorder setup
    vr_ffmpegPath      = cfg.GetString( RecorderFfmpegPathKey,  DefaultFfmpegPath);
    vr_ffmpegOpts      = cfg.GetString( RecorderFfmpegOptsKey,  DefaultFfmpegOptions);
    vr_ffmpegDir       = cfg.GetString( RecorderFfmpegDirKey,   DefaultRecordingDirectory);
    vr_framewidth      = cfg.GetInteger(RecorderFrameWidthKey,  DefaultRecorderFrameWidth);
    vr_frameheight     = cfg.GetInteger(RecorderFrameHeightKey, DefaultRecorderFrameHeight);
    vr_framerate       = cfg.GetInteger(RecorderFrameRateKey,   DefaultRecorderFrameRate);
    vr_sampleRate      = cfg.GetInteger(RecorderSampleRateKey,  DefaultRecorderSampleRate);
    vr_audioChans      = cfg.GetInteger(RecorderAudioChansKey,  DefaultRecorderAudioChans);
    vr_minimumSpaceMiB = cfg.GetInteger(RecorderMinSpaceKey,  1024);
    PString opts = vr_ffmpegOpts;
    PStringStream frameSize; frameSize << vr_framewidth << "x" << vr_frameheight;
    PStringStream frameRate; frameRate << vr_framerate;
    PStringStream outFile; outFile << vr_ffmpegDir << PATH_SEPARATOR << "%o";
    opts.Replace("%F",frameSize,TRUE,0);
    opts.Replace("%R",frameRate,TRUE,0);
    opts.Replace("%S",PString(vr_sampleRate),TRUE,0);
    opts.Replace("%C",PString(vr_audioChans),TRUE,0);
    opts.Replace("%O",outFile,TRUE,0);
    PStringStream tmp;
    tmp << vr_ffmpegPath << " " << opts;
    ffmpegCall=tmp;
  }

  // get WAV file played to a user when they enter a conference
  connectingWAVFile = cfg.GetString(ConnectingWAVFileKey, DefaultConnectingWAVFile);

  // get WAV file played to a conference when a new user enters
  enteringWAVFile = cfg.GetString(EnteringWAVFileKey, DefaultEnteringWAVFile);

  // get WAV file played to a conference when a new user enters
  leavingWAVFile = cfg.GetString(LeavingWAVFileKey, DefaultLeavingWAVFile);

  // Create the config page - general
  GeneralPConfigPage * rsrc = new GeneralPConfigPage(*this, "Parameters", "Parameters", authSettings);
  OnCreateConfigPage(cfg, *rsrc);
  httpNameSpace.AddResource(rsrc, PHTTPSpace::Overwrite);

  // Create the config page - managing users
  httpNameSpace.AddResource(new ManagingUsersPConfigPage(*this, "ManagingUsers", "Managing Users", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - managing groups
  httpNameSpace.AddResource(new ManagingGroupsPConfigPage(*this, "ManagingGroups", "Managing Groups", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - control codes
  httpNameSpace.AddResource(new ControlCodesPConfigPage(*this, "ControlCodes", "Control Codes", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - room codes
  httpNameSpace.AddResource(new RoomCodesPConfigPage(*this, "RoomCodes", "Room Codes", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - h323
  httpNameSpace.AddResource(new H323PConfigPage(*this, "H323Parameters", "H323 Parameters", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - sip
  httpNameSpace.AddResource(new SIPPConfigPage(*this, "SIPParameters", "SIP Parameters", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - h323 endpoints
  httpNameSpace.AddResource(new H323EndpointsPConfigPage(*this, "H323EndpointsParameters", "H323 Endpoints", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - sip endpoints
  httpNameSpace.AddResource(new SipEndpointsPConfigPage(*this, "SipEndpointsParameters", "SIP Endpoints", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - video
#if OPENMCU_VIDEO
  httpNameSpace.AddResource(new VideoPConfigPage(*this, "VideoParameters", "Video", authSettings), PHTTPSpace::Overwrite);
#endif

  // Create the config page - record
  //httpNameSpace.AddResource(new RecordPConfigPage(*this, "RecordParameters", "Parameters", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - sip room acccess
  httpNameSpace.AddResource(new RoomAccessSIPPConfigPage(*this, "RoomAccess", "RoomAccess", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - sip proxy servers
  httpNameSpace.AddResource(new ProxySIPPConfigPage(*this, "ProxyServers", "ProxyServers", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - receive sound codecs
  httpNameSpace.AddResource(new CodecsPConfigPage(*this, "ReceiveSoundCodecs", "RECEIVE_SOUND", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - transmit sound codecs
  httpNameSpace.AddResource(new CodecsPConfigPage(*this, "TransmitSoundCodecs", "TRANSMIT_SOUND", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - receive video codecs
  httpNameSpace.AddResource(new CodecsPConfigPage(*this, "ReceiveVideoCodecs", "RECEIVE_VIDEO", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - transmit video codecs
  httpNameSpace.AddResource(new CodecsPConfigPage(*this, "TransmitVideoCodecs", "TRANSMIT_VIDEO", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - sip sound codecs
  httpNameSpace.AddResource(new CodecsPConfigPage(*this, "SipSoundCodecs", "TRANSMIT_SOUND", authSettings), PHTTPSpace::Overwrite);

  // Create the config page - sip video codecs
  httpNameSpace.AddResource(new CodecsPConfigPage(*this, "SipVideoCodecs", "TRANSMIT_VIDEO", authSettings), PHTTPSpace::Overwrite);

  // Create the status page
  httpNameSpace.AddResource(new MainStatusPage(*this, authConference), PHTTPSpace::Overwrite);

  // Create invite conference page
  httpNameSpace.AddResource(new InvitePage(*this, authConference), PHTTPSpace::Overwrite);

  // Create room selection page
  httpNameSpace.AddResource(new SelectRoomPage(*this, authConference), PHTTPSpace::Overwrite);

  // Create video recording directory browser page:
  httpNameSpace.AddResource(new RecordsBrowserPage(*this, authConference), PHTTPSpace::Overwrite);

#if USE_LIBJPEG
  // Create JPEG frame via HTTP
  httpNameSpace.AddResource(new JpegFrameHTTP(*this, authConference), PHTTPSpace::Overwrite);
#endif

  httpNameSpace.AddResource(new InteractiveHTTP(*this, authConference), PHTTPSpace::Overwrite);

  // Add log file links
/*
  if (!systemLogFileName && (systemLogFileName != "-")) {
    httpNameSpace.AddResource(new PHTTPFile("logfile.txt", systemLogFileName, authority));
    httpNameSpace.AddResource(new PHTTPTailFile("tail_logfile", systemLogFileName, authority));
  }
*/  
  httpNameSpace.AddResource(new WelcomePage(*this, authConference), PHTTPSpace::Overwrite);

  // create monitoring page
  PString monitorText =
#  ifdef GIT_REVISION
#    define _QUOTE_MACRO_VALUE1(x) #x
#    define _QUOTE_MACRO_VALUE(x) _QUOTE_MACRO_VALUE1(x)
                        (PString("OpenMCU REVISION ") + _QUOTE_MACRO_VALUE(GIT_REVISION) +"\n\n") +
#    undef _QUOTE_MACRO_VALUE
#    undef _QUOTE_MACRO_VALUE1
#  endif
                        "<!--#equival monitorinfo-->"
                        "<!--#equival mcuinfo-->";
  httpNameSpace.AddResource(new PServiceHTTPString("monitor.txt", monitorText, "text/plain", authConference), PHTTPSpace::Overwrite);

  // adding web server links (eg. images):
#ifdef SYS_RESOURCE_DIR
#  define WEBSERVER_LINK(r1) httpNameSpace.AddResource(new PHTTPFile(r1, PString(SYS_RESOURCE_DIR) + PATH_SEPARATOR + r1), PHTTPSpace::Overwrite)
#  define WEBSERVER_LINK_MIME(mt1,r1) httpNameSpace.AddResource(new PHTTPFile(r1, PString(SYS_RESOURCE_DIR) + PATH_SEPARATOR + r1, mt1), PHTTPSpace::Overwrite)
#  define WEBSERVER_LINK_MIME_CFG(mt1,r1) httpNameSpace.AddResource(new PHTTPFile(r1, PString(SYS_CONFIG_DIR) + PATH_SEPARATOR + r1, mt1), PHTTPSpace::Overwrite)
#else
#  define WEBSERVER_LINK(r1) httpNameSpace.AddResource(new PHTTPFile(r1), PHTTPSpace::Overwrite)
#  define WEBSERVER_LINK_MIME(mt1,r1) httpNameSpace.AddResource(new PHTTPFile(r1, r1, mt1), PHTTPSpace::Overwrite)
#  define WEBSERVER_LINK_MIME_CFG(mt1,r1) httpNameSpace.AddResource(new PHTTPFile(r1, r1, mt1), PHTTPSpace::Overwrite)
#endif
#define WEBSERVER_LINK_LOGS(mt1,r1) httpNameSpace.AddResource(new PHTTPFile(r1, PString(SERVER_LOGS) + PATH_SEPARATOR + r1, mt1), PHTTPSpace::Overwrite)
  WEBSERVER_LINK_MIME("text/javascript"          , "control.js");
  WEBSERVER_LINK_MIME("text/javascript"          , "status.js");
  WEBSERVER_LINK_MIME("text/javascript"          , "locale_ru.js");
  WEBSERVER_LINK_MIME("text/javascript"          , "locale_en.js");
  WEBSERVER_LINK_MIME("text/javascript"          , "locale_uk.js");
  WEBSERVER_LINK_MIME("text/javascript"          , "locale_jp.js");
  WEBSERVER_LINK_MIME("text/css"                 , "main.css");
  WEBSERVER_LINK_MIME("image/png"                , "s15_ch.png");
  WEBSERVER_LINK_MIME("image/gif"                , "i15_getNoVideo.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "openmcu.ru_vad_vad.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "openmcu.ru_vad_disable.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "openmcu.ru_vad_chosenvan.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i15_inv.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "openmcu.ru_launched_Ypf.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i20_close.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i20_vad.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i20_vad2.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i20_static.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i20_plus.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i24_shuff.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i24_left.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i24_right.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i24_mix.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i24_clr.gif");
  WEBSERVER_LINK_MIME("image/gif"                , "i24_revert.gif");
  WEBSERVER_LINK_MIME("image/vnd.microsoft.icon" , "openmcu.ico");
  WEBSERVER_LINK_MIME("image/png"                , "openmcu.ru_logo_text.png");
  WEBSERVER_LINK_MIME("image/png"                , "menu_left.png");
#if USE_LIBJPEG
  WEBSERVER_LINK_MIME_CFG("image/jpeg"               , "logo.jpeg");
#endif
  WEBSERVER_LINK_MIME("image/png"                , "i16_close_gray.png");
  WEBSERVER_LINK_MIME("image/png"                , "i16_close_red.png");
  WEBSERVER_LINK_MIME("image/png"                , "i32_lock.png");
  WEBSERVER_LINK_MIME("image/png"                , "i32_lockopen.png");

  for(PINDEX i=-1; i<rotationLevel; i++)
  {
    PString s="trace";
    if(i>=0) s+=PString(i);
    s+=".txt";
    WEBSERVER_LINK_LOGS("application/octet-stream", s);
  }

  // set up the HTTP port for listening & start the first HTTP thread
  if (ListenForHTTP((WORD)cfg.GetInteger(HttpPortKey, DefaultHTTPPort)))
    PSYSTEMLOG(Info, "Opened master socket for HTTP: " << httpListeningSocket->GetPort());
  else {
    PSYSTEMLOG(Fatal, "Cannot run without HTTP port: " << httpListeningSocket->GetErrorText());
    return FALSE;
  }

  if(cfg.GetBoolean(CreateEmptyRoomKey, FALSE))
  { GetEndpoint().GetConferenceManager().MakeAndLockConference(defaultRoomName);
    GetEndpoint().GetConferenceManager().UnlockConference();
  }

  // refresh Address Book
  PWaitAndSignal m(GetEndpoint().GetConferenceManager().GetConferenceListMutex());
  ConferenceListType & conferenceList = GetEndpoint().GetConferenceManager().GetConferenceList();
  ConferenceListType::iterator r;
  for (r = conferenceList.begin(); r != conferenceList.end(); ++r)
    if(r->second) r->second->RefreshAddressBook();

  PSYSTEMLOG(Info, "Service " << GetName() << ' ' << initMsg);
  return TRUE;
}

//////////////////////////////////////////////////////////////////////////////////////////////

void OpenMCU::OnConfigChanged()
{
}

PString OpenMCU::GetNewRoomNumber()
{
  static PAtomicInteger number(100);
  return PString(PString::Unsigned, ++number);
}

void OpenMCU::LogMessage(const PString & str)
{
  static PMutex logMutex;
  static PTextFile logFile;

  PTime now;
  PString msg = now.AsString("dd/MM/yyyy") & str;
  logMutex.Wait();

  if (!logFile.IsOpen()) {
    if(!logFile.Open(logFilename, PFile::ReadWrite))
    {
      PTRACE(1,"OpenMCU\tCan not open log file: " << logFilename << "\n" << msg << flush);
      logMutex.Signal();
      return;
    }
    if(!logFile.SetPosition(0, PFile::End))
    {
      PTRACE(1,"OpenMCU\tCan not change log position, log file name: " << logFilename << "\n" << msg << flush);
      logFile.Close();
      logMutex.Signal();
      return;
    }
  }

  if(!logFile.WriteLine(msg))
  {
    PTRACE(1,"OpenMCU\tCan not write to log file: " << logFilename << "\n" << msg << flush);
  }
  logFile.Close();
  logMutex.Signal();
}

void OpenMCU::LogMessageHTML(PString str)
{ // de-html :) special for palexa, http://openmcu.ru/forum/index.php/topic,351.msg6240.html#msg6240
  PString str2, roomName;
  PINDEX tabPos=str.Find("\t");
  if(tabPos!=P_MAX_INDEX)
  {
    roomName=str.Left(tabPos);
    str=str.Mid(tabPos+1,P_MAX_INDEX);
  }
  if(str.GetLength()>8)
  {
    if(str[1]==':') str=PString("0")+str;
    if(!roomName.IsEmpty()) str=str.Left(8)+" "+roomName+str.Mid(9,P_MAX_INDEX);
  }
  BOOL tag=FALSE;
  for (PINDEX i=0; i< str.GetLength(); i++)
  if(str[i]=='<') tag=TRUE;
  else if(str[i]=='>') tag=FALSE;
  else if(!tag) str2+=str[i];
  LogMessage(str2);
}

ConferenceManager * OpenMCU::CreateConferenceManager()
{
  return new ConferenceManager();
}

OpenMCUH323EndPoint * OpenMCU::CreateEndPoint(ConferenceManager & manager)
{
  return new OpenMCUH323EndPoint(manager);
}

PString OpenMCU::GetEndpointParamFromUrl(PString param, PString addr)
{
  PString user, host, section;
  PString newParam;
  PStringArray options;

  MCUURL url(addr);
  user = url.GetUserName();
  host = url.GetHostName();
  if(url.GetScheme() == "h323")
  {
    section = "H323 Endpoints";
    options = h323EndpointOptionsOrder;
  }
  else if(url.GetScheme() == "sip")
  {
    section = "SIP Endpoints";
    options = sipEndpointOptionsOrder;
  } else {
    return "";
  }

  MCUConfig cfg(section);
  PStringList keys = cfg.GetKeys();
  PINDEX index = keys.GetStringsIndex(user+"@"+host);
  if(index == P_MAX_INDEX && host != "") index = keys.GetStringsIndex(host);
  if(index == P_MAX_INDEX && host != "") index = keys.GetStringsIndex("@"+host);
  if(index == P_MAX_INDEX && user != "") index = keys.GetStringsIndex(user);
  if(index == P_MAX_INDEX && user != "") index = keys.GetStringsIndex(user+"@");

  if(index != P_MAX_INDEX)
  {
    PStringArray params = cfg.GetString(keys[index]).Tokenise(",");
    if(options.GetStringsIndex(param) != P_MAX_INDEX)
      newParam = params[options.GetStringsIndex(param)];
  }
  if(newParam == "") index = keys.GetStringsIndex("*");
  if(index != P_MAX_INDEX)
  {
    PStringArray params = cfg.GetString(keys[index]).Tokenise(",");
    if(options.GetStringsIndex(param) != P_MAX_INDEX)
      newParam = params[options.GetStringsIndex(param)];
  }

  return newParam;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUURL::MCUURL(PString str)
{
  PINDEX delim1 = str.FindLast("[");
  PINDEX delim2 = str.FindLast("]");
  PINDEX delim3 = str.FindLast("<sip:");
  PINDEX delim4 = str.FindLast(">");

  if(delim3 != P_MAX_INDEX && delim4 != P_MAX_INDEX)
  {
    displayName = str.Left(delim3-1);
    displayName.Replace("\"","",TRUE,0);
    partyUrl = str.Mid(delim3+1, delim4-delim3-1);
  }
  else if(delim1 != P_MAX_INDEX && delim2 != P_MAX_INDEX)
  {
    displayName = str.Left(delim1-1);
    partyUrl = str.Mid(delim1+1, delim2-delim1-1);
  } else {
    partyUrl = str;
  }

  if(partyUrl.Left(4) == "sip:") URLScheme = "sip";
  else if(partyUrl.Left(5) == "h323:") URLScheme = "h323";
  else { partyUrl = "h323:"+partyUrl; URLScheme = "h323"; }

  Parse((const char *)partyUrl, URLScheme);
  // parse old H.323 scheme
  if(URLScheme == "h323" && partyUrl.Left(5) != "h323" && partyUrl.Find("@") == P_MAX_INDEX &&
     hostname == "" && username != "")
  {
    hostname = username;
    username = "";
  }

  memberName = displayName+" ["+partyUrl+"]";

  if(URLScheme == "sip")
    urlId = username+"@"+hostname;
  else if(URLScheme == "h323")
    urlId = displayName+"@"+hostname;

  if(URLScheme == "sip")
  {
    sipProto = "*";
    if(partyUrl.Find("transport=udp") != P_MAX_INDEX) sipProto = "udp";
    if(partyUrl.Find("transport=tcp") != P_MAX_INDEX) sipProto = "tcp";
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

ExternalVideoRecorderThread::ExternalVideoRecorderThread(const PString & _roomName)
  : roomName(_roomName)
{
  state = 0;

  PStringStream t; t << roomName << "__" // fileName format: room101__2013-0516-1058270__704x576x10
    << PTime().AsString("yyyy-MMdd-hhmmssu", PTime::Local) << "__"
    << OpenMCU::Current().vr_framewidth << "x"
    << OpenMCU::Current().vr_frameheight << "x"
    << OpenMCU::Current().vr_framerate;
  fileName = t;

  t = OpenMCU::Current().ffmpegCall;
  t.Replace("%o",fileName,TRUE,0);
  PString audio, video;
#ifdef _WIN32
  audio = "\\\\.\\pipe\\sound_" + roomName;
  video = "\\\\.\\pipe\\video_" + roomName;
#else
#  ifdef SYS_PIPE_DIR
  audio = PString(SYS_PIPE_DIR)+"/sound." + roomName;
  video = PString(SYS_PIPE_DIR)+"/video." + roomName;
#  else
  audio = "sound." + roomName;
  video = "video." + roomName;
#  endif
#endif
  t.Replace("%A",audio,TRUE,0);
  t.Replace("%V",video,TRUE,0);
  PINDEX i;
#ifdef _WIN32
  PString t2; for(i=0;i<t.GetLength();i++) { char c=t[i]; if(c=='\\') t2+='\\'; t2+=c; } t=t2;
#endif
  if(!ffmpegPipe.Open(t, /* PPipeChannel::ReadWrite */ PPipeChannel::WriteOnly, FALSE, FALSE)) { state=2; return; }
  if(!ffmpegPipe.IsOpen()) { state=2; return; }
  for (i=0;i<100;i++) if(ffmpegPipe.IsRunning()) {state=1; break; } else PThread::Sleep(12);
  if(state != 1) { ffmpegPipe.Close(); state=2; return; }

  PTRACE(2,"EVRT\tStarted new external recording thread for room " << roomName << ", CL: " << t);
//  char buf[100]; while(ffmpegPipe.Read(buf, 100)) PTRACE(1,"EVRT\tRead data: " << buf);//<--thread overloaded, never FALSE?
}

ExternalVideoRecorderThread::~ExternalVideoRecorderThread()
{
  state=3;
  if(ffmpegPipe.IsOpen())
  {
    if(ffmpegPipe.IsRunning())
    {
      ffmpegPipe.Write("q\r\n",3);
#     ifdef _WIN32
        ffmpegPipe.WaitForTermination(500); // linux: assertion "unimplemented" - works like no parameter passed
#     else
        ffmpegPipe.Kill(SIGINT); // for ffmpeg to finalize record and flush output buffers
        unsigned i; for(i=0;i<25;i++) while(ffmpegPipe.IsRunning()) PThread::Sleep(20);
#     endif
    }
    ffmpegPipe.Close();
  }
  state=4;
}

// End of File ///////////////////////////////////////////////////////////////

