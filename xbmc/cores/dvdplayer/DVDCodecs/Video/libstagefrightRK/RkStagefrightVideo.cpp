/*
 *      Copyright (C) 2013 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
/***************************************************************************/

//#define DEBUG_VERBOSE 1

#include "system.h"
#include "system_gl.h"

#include "DVDVideoCodecStageFright.h"
#include "RkStagefrightVideo.h"
#include "StageFrightVideoPrivate.h"

#include "android/activity/XBMCApp.h"
#include "guilib/GraphicContext.h"
#include "DVDClock.h"
#include "utils/log.h"
#include "utils/fastmemcpy.h"
#include "utils/StringUtils.h"
#include "threads/Thread.h"
#include "threads/Event.h"
#include "settings/AdvancedSettings.h"
#include "DVDCodecs/DVDCodecInterface.h"

#include <new>

#define RINT(x) ((x) >= 0 ? ((int)((x) + 0.5)) : ((int)((x) - 0.5)))

#define CLASSNAME "CRkStagefrightVideo"

const char *MEDIA_MIMETYPE_VIDEO_WMV  = "video/x-ms-wmv";
const char *MEDIA_MIMETYPE_VIDEO_VC1  = "video/vc1";
const char *MEDIA_MIMETYPE_VIDEO_VP6  = "video/vp6";

const int kKeyVC1ExtraSize      = 'vc1e';  // vc1 extra data size
const int kKeyVC1               = 'vc1c';  // vc1 codec config info
const int kKeyHVCC              = 'hvcc';

#define XMEDIA_BITSTREAM_START_CODE         (0x42564b52)

using namespace android;

static int64_t pts_dtoi(double pts)
{
  return (int64_t)(pts);
}

static double pts_itod(int64_t pts)
{
  return (double)pts;
}

/***********************************************************/

class CStageFrightMediaSource : public MediaSource
{
public:
  CStageFrightMediaSource(CRkStageFrightVideo *priv, sp<MetaData> meta)
  {
    p = priv;
    source_meta = meta;
  }

  virtual sp<MetaData> getFormat()
  {
    return source_meta;
  }

  virtual status_t start(MetaData *params)
  {
    return OK;
  }

  virtual status_t stop()
  {
    return OK;
  }

  virtual status_t read(MediaBuffer **buffer,
                        const MediaSource::ReadOptions *options)
  {
    Frame *frame;
    status_t ret;
    *buffer = NULL;
    int64_t time_us = -1;
    MediaSource::ReadOptions::SeekMode mode;
    
    if (options && options->getSeekTo(&time_us, &mode))
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s: reading source(%d): seek:%llu\n", CLASSNAME,in_queue.size(), time_us);
#endif
    }
    else
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s: reading source(%d)\n", CLASSNAME,in_queue.size());
#endif
    }

    p->in_mutex.lock();
    while (p->m_in_queue.empty() && p->decode_thread)
      p->in_condition.wait(p->in_mutex);

    if (p->m_in_queue.empty())
    {
      p->in_mutex.unlock();
      return VC_ERROR;
    }
    
    std::map<int64_t,Frame*>::iterator it = p->m_in_queue.begin();
    frame = it->second;
    ret = frame->status;
    *buffer = frame->medbuf;

    p->m_in_queue.erase(it);
    p->in_mutex.unlock();

#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> exiting reading source(%d); pts:%llu\n", in_queue.size(),frame->pts);
#endif

    free(frame);

    return ret;
  }

private:
  sp<MetaData> source_meta;
  CRkStageFrightVideo *p;
};

/********************************************/

class CStageFrightDecodeThread : public CThread
{
protected:
  CRkStageFrightVideo *p;

public:
  CStageFrightDecodeThread(CRkStageFrightVideo *priv)
  : CThread("CStageFrightDecodeThread")
  , p(priv)
  {}
  
  void OnStartup()
  {
  #if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s: entering decode thread\n", CLASSNAME);
  #endif
  }
  
  void OnExit()
  {
  #if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s: exited decode thread\n", CLASSNAME);
  #endif
  }
  
  void Process()
  {
    Frame* frame;
    int32_t w, h, dw, dh;
    int decode_done = 0;
    int32_t keyframe = 0;
    int32_t unreadable = 0;
    MediaSource::ReadOptions readopt;
    // GLuint texid;

    //SetPriority(THREAD_PRIORITY_ABOVE_NORMAL);
    do
    {
      #if defined(DEBUG_VERBOSE)
      unsigned int time = XbmcThreads::SystemClockMillis();
      CLog::Log(LOGDEBUG, "%s: >>> Handling frame\n", CLASSNAME);
      #endif
      p->cur_frame = NULL;
      frame = (Frame*)malloc(sizeof(Frame));
      if (!frame) 
      {
        decode_done   = 1;
        continue;
      }

      frame->eglimg = EGL_NO_IMAGE_KHR;
      frame->medbuf = NULL;
      if (p->resetting)
      {
        CDVDClock *playerclock = CDVDClock::GetMasterClock();
        if (playerclock)
          readopt.setSeekTo(pts_dtoi(playerclock->GetClock()));
        else
          readopt.setSeekTo(0);
        p->resetting = false;
      }
      frame->status = p->m_stfdecoder->read(&frame->medbuf, &readopt);
      readopt.clearSeekTo();
      
      if (frame->status == OK)
      {
        if (!frame->medbuf->graphicBuffer().get())  // hw buffers
        {
          if (frame->medbuf->range_length() == sizeof(VPU_FRAME))
            frame->format = RENDER_FMT_STFBUF;
          else
            frame->status = BAD_TYPE;
        }
        else
          frame->status = BAD_TYPE;
      }

      if (frame->status == OK)
      {
        frame->width = p->width;
        frame->height = p->height;
        frame->pts = 0;

        sp<MetaData> outFormat = p->m_stfdecoder->getFormat();
        outFormat->findInt32(kKeyWidth , &w);
        outFormat->findInt32(kKeyHeight, &h);

        if (!outFormat->findInt32(kKeyDisplayWidth , &dw))
          dw = w;
        if (!outFormat->findInt32(kKeyDisplayHeight, &dh))
          dh = h;

        if (!outFormat->findInt32(kKeyIsSyncFrame, &keyframe))
          keyframe = 0;
        if (!outFormat->findInt32(kKeyIsUnreadable, &unreadable))
          unreadable = 0;

        frame->pts = 0;
        frame->medbuf->meta_data()->findInt64(kKeyTime, &(frame->pts));
      }
      else if (frame->status == INFO_FORMAT_CHANGED)
      {
        int32_t cropLeft, cropTop, cropRight, cropBottom;
        sp<MetaData> outFormat = p->m_stfdecoder->getFormat();

        outFormat->findInt32(kKeyWidth , &p->width);
        outFormat->findInt32(kKeyHeight, &p->height);

        cropLeft = cropTop = cropRight = cropBottom = 0;
        if (!outFormat->findRect(kKeyCropRect, &cropLeft, &cropTop, &cropRight, &cropBottom))
        {
          p->x = 0;
          p->y = 0;
        }
        else
        {
          p->x = cropLeft;
          p->y = cropTop;
          p->width = cropRight - cropLeft + 1;
          p->height = cropBottom - cropTop + 1;
        }
        outFormat->findInt32(kKeyColorFormat, &p->videoColorFormat);
        if (!outFormat->findInt32(kKeyStride, &p->videoStride))
          p->videoStride = p->width;
        if (!outFormat->findInt32(kKeySliceHeight, &p->videoSliceHeight))
          p->videoSliceHeight = p->height;

#if defined(DEBUG_VERBOSE)
        CLog::Log(LOGDEBUG, ">>> new format col:%d, w:%d, h:%d, sw:%d, sh:%d, ctl:%d,%d; cbr:%d,%d\n", p->videoColorFormat, p->width, p->height, p->videoStride, p->videoSliceHeight, cropTop, cropLeft, cropBottom, cropRight);
#endif

        if (frame->medbuf)
          frame->medbuf->release();
        frame->medbuf = NULL;
        free(frame);
        continue;
      }
      else
      {
        CLog::Log(LOGERROR, "%s - decoding error (%d)\n", CLASSNAME,frame->status);
        if (frame->medbuf)
          frame->medbuf->release();
        frame->medbuf = NULL;
        free(frame);
        continue;
      }

      if (frame->format == RENDER_FMT_STFBUF)
      {
        p->out_mutex.lock();
        p->outbuf_queue.push_back(frame);

#if defined(DEBUG_VERBOSE)
        CLog::Log(LOGDEBUG, "%s: >>> pushed OUT frame; w:%d, h:%d, dw:%d, dh:%d, kf:%d, ur:%d, buf:%p, tm:%d\n", CLASSNAME, frame->width, frame->height, dw, dh, keyframe, unreadable, frame->medbuf, XbmcThreads::SystemClockMillis() - time);
#endif

        while (p->outbuf_queue.size() >= OUTBUFCOUNT)
          p->out_condition.wait(p->out_mutex);
        p->out_mutex.unlock();
        continue;
      }
    #if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s: >>> pushed OUT frame; w:%d, h:%d, dw:%d, dh:%d, kf:%d, ur:%d, img:%p, tm:%d\n", CLASSNAME, frame->width, frame->height, dw, dh, keyframe, unreadable, frame->eglimg, XbmcThreads::SystemClockMillis() - time);
    #endif

      p->out_mutex.lock();
      p->cur_frame = frame;
      while (p->cur_frame)
        p->out_condition.wait(p->out_mutex);
      p->out_mutex.unlock();
    }
    while (!decode_done && !m_bStop);
  }
};

/***********************************************************/

CRkStageFrightVideo::CRkStageFrightVideo(CDVDCodecInterface* interface)
  : decode_thread(NULL), m_mediasource(NULL)
  , m_framecount(0)
  , m_omxclient(NULL), m_stfdecoder(NULL)
  , drop_state(false), resetting(false)
  , cur_frame(NULL), prev_frame(NULL)
  , width(-1), height(-1)
  , extrasize(0), extradata(NULL)
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::ctor: %d\n", CLASSNAME, sizeof(CStageFrightVideo));
#endif
  p = new CStageFrightVideoPrivate;
  m_g_application = interface->GetApplication();
  m_g_applicationMessenger = interface->GetApplicationMessenger();
  m_g_Windowing = interface->GetWindowSystem();
  m_g_advancedSettings = interface->GetAdvancedSettings();
}

CRkStageFrightVideo::~CRkStageFrightVideo()
{
  SAFE_DELETE(p);
}

bool CRkStageFrightVideo::Open(CDVDStreamInfo &hints)
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Open: ed:%d as:%f fa:%d\n", CLASSNAME, hints.extrasize, hints.aspect, hints.forced_aspect);
#endif

  CSingleLock lock(g_graphicsContext);

  // stagefright crashes with null size. Trap this...
  if (!hints.width || !hints.height)
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"null size, cannot handle");
    return false;
  }
  codec     = hints.codec;
  width     = hints.width;
  height    = hints.height;
  if (!hints.forced_aspect)
    p->aspect_ratio = hints.aspect;
  else
    p->aspect_ratio = 1.0;

  sp<MetaData> outFormat;
  int32_t cropLeft, cropTop, cropRight, cropBottom;
  //Vector<String8> matchingCodecs;

  m_metadata = new MetaData;
  if (m_metadata == NULL)
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"cannot allocate MetaData");
    return false;
  }

  const char* mimetype;
  switch (hints.codec)
  {
    case AV_CODEC_ID_HEVC:
      if (m_g_advancedSettings->m_stagefrightConfig.useHEVCcodec == 0)
        return false;
      mimetype = "video/hevc";
      m_metadata->setData(kKeyHVCC, kTypeAVCC, hints.extradata, hints.extrasize);
      break;
  case AV_CODEC_ID_H264:
//  case AV_CODEC_ID_H264MVC:
      if (m_g_advancedSettings->m_stagefrightConfig.useAVCcodec == "0"
          || (m_g_advancedSettings->m_stagefrightConfig.useAVCcodec == "sd" && hints.width > 800)
          || (m_g_advancedSettings->m_stagefrightConfig.useAVCcodec == "hd" && hints.width <= 800))
        return false;
      mimetype = "video/avc";
      m_metadata->setData(kKeyAVCC, kTypeAVCC, hints.extradata, hints.extrasize);
    break;
  case AV_CODEC_ID_MPEG4:
      if (m_g_advancedSettings->m_stagefrightConfig.useMP4codec == "0"
          || (m_g_advancedSettings->m_stagefrightConfig.useMP4codec == "sd" && hints.width > 800)
          || (m_g_advancedSettings->m_stagefrightConfig.useMP4codec == "hd" && hints.width <= 800))
        return false;
    mimetype = "video/mp4v-es";
    break;
  case AV_CODEC_ID_MPEG2VIDEO:
      if (m_g_advancedSettings->m_stagefrightConfig.useMPEG2codec == "0"
          || (m_g_advancedSettings->m_stagefrightConfig.useMPEG2codec == "sd" && hints.width > 800)
          || (m_g_advancedSettings->m_stagefrightConfig.useMPEG2codec == "hd" && hints.width <= 800))
        return false;
    mimetype = "video/mpeg2";
    break;
  case AV_CODEC_ID_VP3:
  case AV_CODEC_ID_VP6:
  case AV_CODEC_ID_VP6F:
      if (m_g_advancedSettings->m_stagefrightConfig.useVPXcodec == "0"
          || (m_g_advancedSettings->m_stagefrightConfig.useVPXcodec == "sd" && hints.width > 800)
          || (m_g_advancedSettings->m_stagefrightConfig.useVPXcodec == "hd" && hints.width <= 800))
        return false;
    mimetype = "video/vp6";
    break;
    case AV_CODEC_ID_VP8:
      if (m_g_advancedSettings->m_stagefrightConfig.useVPXcodec == "0"
          || (m_g_advancedSettings->m_stagefrightConfig.useVPXcodec == "sd" && hints.width > 800)
          || (m_g_advancedSettings->m_stagefrightConfig.useVPXcodec == "hd" && hints.width <= 800))
        return false;
      mimetype = "video/x-vnd.on2.vp8";
      break;
  case AV_CODEC_ID_VC1:
  //case AV_CODEC_ID_WMV3:
      if (m_g_advancedSettings->m_stagefrightConfig.useVC1codec == "0"
          || (m_g_advancedSettings->m_stagefrightConfig.useVC1codec == "sd" && hints.width > 800)
          || (m_g_advancedSettings->m_stagefrightConfig.useVC1codec == "hd" && hints.width <= 800))
        return false;
    mimetype = "video/vc1";
    extrasize = hints.extrasize;
    extradata = hints.extradata;
    m_metadata->setInt32(kKeyVC1ExtraSize, hints.extrasize);
    break;
  default:
    return false;
    break;
  }

  m_metadata->setCString(kKeyMIMEType, mimetype);
  m_metadata->setInt32(kKeyWidth, width);
  m_metadata->setInt32(kKeyHeight, height);

  android::ProcessState::self()->startThreadPool();

  m_mediasource    = new CStageFrightMediaSource(this, m_metadata);
  m_omxclient    = new OMXClient;

  if (m_mediasource == NULL || m_omxclient == NULL)
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Cannot obtain source / client");
    return false;
  }

  if (m_omxclient->connect() !=  OK)
  {
    delete m_omxclient;
    m_omxclient = NULL;
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Cannot connect OMX client");
    return false;
  }

  m_stfdecoder  = OMXCodec::Create(m_omxclient->interface(), m_metadata,
                                         false, m_mediasource, NULL,
                                         OMXCodec::kSoftwareCodecsOnly,
                                         NULL
                                         );

  if (!(m_stfdecoder != NULL && m_stfdecoder->start() ==  OK))
  {
    m_stfdecoder = NULL;
    return false;
  }

  outFormat = m_stfdecoder->getFormat();

  if (!outFormat->findInt32(kKeyWidth, &width) || !outFormat->findInt32(kKeyHeight, &height)
        || !outFormat->findInt32(kKeyColorFormat, &videoColorFormat))
    return false;

  const char *component;
  if (outFormat->findCString(kKeyDecoderComponent, &component))
  {
    CLog::Log(LOGDEBUG, "%s::%s - component: %s\n", CLASSNAME, __func__, component);
    
    //Blacklist
    if (!strncmp(component, "OMX.google", 10))
    {
      // On some platforms, software decoders are returned anyway
      CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Blacklisted component (software)");
      goto fail;
    }
    else if (!strncmp(component, "OMX.Nvidia.mp4.decode", 21) && m_g_advancedSettings->m_stagefrightConfig.useMP4codec != "1")
    {
      // Has issues with some XVID encoded MP4. Only fails after actual decoding starts...
      CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Blacklisted component (MP4)");
      goto fail;
    }
  }

  cropLeft = cropTop = cropRight = cropBottom = 0;
  if (!outFormat->findRect(kKeyCropRect, &cropLeft, &cropTop, &cropRight, &cropBottom))
  {
    x = 0;
    y = 0;
  }
  else
  {
    x = cropLeft;
    y = cropTop;
    width = cropRight - cropLeft + 1;
    height = cropBottom - cropTop + 1;
  }

  if (!outFormat->findInt32(kKeyStride, &videoStride))
    videoStride = width;
  if (!outFormat->findInt32(kKeySliceHeight, &videoSliceHeight))
    videoSliceHeight = height;
  
  for (int i=0; i<INBUFCOUNT; ++i)
  {
    p->inbuf[i] = new MediaBuffer(300000);
    p->inbuf[i]->setObserver(p);
  }
  
  decode_thread = new CStageFrightDecodeThread(this);
  decode_thread->Create(true /*autodelete*/);

  if (extrasize)
    Decode((uint8_t*)extradata, extrasize, 0, 0);

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, ">>> format col:%d, w:%d, h:%d, sw:%d, sh:%d, ctl:%d,%d; cbr:%d,%d\n", p->videoColorFormat, p->width, p->height, p->videoStride, p->videoSliceHeight, cropTop, cropLeft, cropBottom, cropRight);
#endif

  return true;

fail:
  if (m_stfdecoder != 0)
    m_stfdecoder->stop();
  if (m_omxclient)
  {
    m_omxclient->disconnect();
    delete m_omxclient;
  }
  return false;
}

/*** Decode ***/
int  CRkStageFrightVideo::Decode(uint8_t *pData, int iSize, double dts, double pts)
{
#if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s::Decode - d:%p; s:%d; dts:%f; pts:%f\n", CLASSNAME, pData, iSize, dts, pts);
#endif

  Frame *frame;
  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  int ret = 0;

  if (demuxer_content)
  {
    frame = (Frame*)malloc(sizeof(Frame));
    if (!frame)
      return VC_ERROR;

    frame->status  = OK;
    if (m_g_advancedSettings->m_stagefrightConfig.useInputDTS)
      frame->pts = (dts != DVD_NOPTS_VALUE) ? pts_dtoi(dts) : ((pts != DVD_NOPTS_VALUE) ? pts_dtoi(pts) : 0);
    else
      frame->pts = (pts != DVD_NOPTS_VALUE) ? pts_dtoi(pts) : ((dts != DVD_NOPTS_VALUE) ? pts_dtoi(dts) : 0);

    // No valid pts? libstagefright asserts on this.
    if (frame->pts < 0)
    {
      free(frame);
      return ret;
    }

    if (codec == AV_CODEC_ID_MPEG2VIDEO)
    {
      frame->medbuf = p->getBuffer(demuxer_bytes + sizeof(VPU_BITSTREAM));
      frame->medbuf->set_range(0, demuxer_bytes  + sizeof(VPU_BITSTREAM));
    }
    else
    {
      frame->medbuf = p->getBuffer(demuxer_bytes);
      frame->medbuf->set_range(0, demuxer_bytes);
    }

    if (!frame->medbuf)
    {
      free(frame);
      return VC_ERROR;
    }

    if (codec == AV_CODEC_ID_MPEG2VIDEO)
    {
      VPU_BITSTREAM m2vheader;
      m2vheader.StartCode = XMEDIA_BITSTREAM_START_CODE;
      m2vheader.SliceLength = demuxer_bytes;
      m2vheader.SliceTime.TimeLow = (uint32_t)(frame->pts / 1000);
      m2vheader.SliceTime.TimeHigh = 0;
      m2vheader.SliceType = 0;
      m2vheader.SliceNum = 0;

      fast_memcpy(frame->medbuf->data(), &m2vheader, sizeof(VPU_BITSTREAM));
      fast_memcpy((void*)((long)frame->medbuf->data() + sizeof(VPU_BITSTREAM)), demuxer_content, demuxer_bytes);
    }
    else
      fast_memcpy(frame->medbuf->data(), demuxer_content, demuxer_bytes);
    frame->medbuf->meta_data()->clear();
    frame->medbuf->meta_data()->setInt64(kKeyTime, frame->pts);
    
    in_mutex.lock();
    m_framecount++;
    m_in_queue.insert(std::pair<int64_t, Frame*>(m_framecount, frame));
    in_condition.notify();
    in_mutex.unlock();
  }

  if (p->inputBufferAvailable() && m_in_queue.size() < INBUFCOUNT)
    ret |= VC_BUFFER;
  else
    usleep(1000);
  if (cur_frame != NULL || outbuf_queue.size())
    ret |= VC_PICTURE;
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Decode: pushed IN frame (%d); tm:%d\n", CLASSNAME,in_queue.size(), XbmcThreads::SystemClockMillis() - time);
#endif

  return ret;
}

bool CRkStageFrightVideo::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
 #if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif
  if (pDvdVideoPicture->format == RENDER_FMT_EGLIMG || pDvdVideoPicture->format == RENDER_FMT_STFBUF)
  {
    ReleaseBuffer(pDvdVideoPicture->stfbuf);
#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s::ClearPicture buf:%p (%d)\n", CLASSNAME, pDvdVideoPicture->stfbuf, XbmcThreads::SystemClockMillis() - time);
#endif
    return true;
  }

  if (prev_frame) {
    if (prev_frame->medbuf)
      prev_frame->medbuf->release();
    free(prev_frame);
    prev_frame = NULL;
  }
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::ClearPicture img:%p (%d)\n", CLASSNAME, pDvdVideoPicture, XbmcThreads::SystemClockMillis() - time);
#endif

  return true;
}

bool CRkStageFrightVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
#if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s::GetPicture\n", CLASSNAME);
  if (p->cycle_time != 0)
    CLog::Log(LOGDEBUG, ">>> cycle dur:%d\n", XbmcThreads::SystemClockMillis() - p->cycle_time);
  p->cycle_time = time;
#endif

  status_t status;

  out_mutex.lock();
  Frame *frame = NULL;
  if (cur_frame)
    frame = cur_frame;
  else if (outbuf_queue.size())
  {
    frame = outbuf_queue.front();
    outbuf_queue.pop_front();
  }
  else
  {
    CLog::Log(LOGERROR, "%s::%s - Error getting frame\n", CLASSNAME, __func__);
    out_condition.notify();
    out_mutex.unlock();
    return false;
  }

  status  = frame->status;

  pDvdVideoPicture->format = frame->format;
  pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
  pDvdVideoPicture->pts = frame->pts;
  pDvdVideoPicture->iWidth  = width;
  pDvdVideoPicture->iHeight = height;
  if (!p->aspect_ratio || p->aspect_ratio == 1.0)
  {
    pDvdVideoPicture->iDisplayWidth = width;
    pDvdVideoPicture->iDisplayHeight = height;
  }
  else
  {
    pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;
    pDvdVideoPicture->iDisplayWidth  = ((int)RINT(pDvdVideoPicture->iHeight * p->aspect_ratio)) & -3;
    if (pDvdVideoPicture->iDisplayWidth > pDvdVideoPicture->iWidth)
    {
      pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
      pDvdVideoPicture->iDisplayHeight = ((int)RINT(pDvdVideoPicture->iWidth / p->aspect_ratio)) & -3;
    }
  }

  pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
  pDvdVideoPicture->stfbuf = NULL;

  if (status != OK)
  {
    CLog::Log(LOGERROR, "%s::%s - Error getting picture from frame(%d)\n", CLASSNAME, __func__,status);
    if (frame->medbuf) {
      frame->medbuf->release();
    }
    free(frame);
    cur_frame = NULL;
    out_condition.notify();
    out_mutex.unlock();
    return false;
  }

  VPU_FRAME *vpuframe = (VPU_FRAME *)((long)frame->medbuf->data() + frame->medbuf->range_offset());
  VPU_FRAME *vpucopy = NULL;
  if (vpuframe->vpumem.phy_addr)
  {
    vpucopy = (VPU_FRAME *)malloc(sizeof(VPU_FRAME));
    memcpy(vpucopy,vpuframe,sizeof(VPU_FRAME));
    VPUMemLink(&vpuframe->vpumem);
    VPUMemDuplicate(&vpucopy->vpumem,&vpuframe->vpumem);
    VPUFreeLinear(&vpuframe->vpumem);
    //      vpucopy = vpuframe;

    VPUMemLink(&vpucopy->vpumem);
    if (vpucopy->FrameWidth == 0)
      vpucopy->FrameWidth = vpucopy->DisplayWidth;
    if (vpucopy->FrameHeight == 0)
      vpucopy->FrameHeight = vpucopy->DisplayHeight;

    frame->medbuf->release();
    SAFE_DELETE(frame);

    if (vpucopy)
    {
      CDVDVideoCodecStageFrightBuffer* stfbuf = new CDVDVideoCodecStageFrightBuffer;
      stfbuf->format = RENDER_FMT_STFBUF;
      stfbuf->subformat = 'rkvp';
      stfbuf->frameWidth = vpucopy->FrameWidth;
      stfbuf->frameHeight = vpucopy->FrameHeight;
      stfbuf->displayWidth = vpucopy->DisplayWidth;
      stfbuf->displayHeight = vpucopy->DisplayHeight;
      stfbuf->buffer = (void*)vpucopy->vpumem.vir_addr;
      stfbuf->context = (void*)vpucopy;
      LockBuffer(stfbuf);

      pDvdVideoPicture->stfbuf = stfbuf;
//#if defined(DEBUG_VERBOSE)
      //CLog::Log(LOGDEBUG, ">>> pic dts:%f, pts:%llu, buf:%p, tm:%d\n", pDvdVideoPicture->dts, pDvdVideoPicture->pts, pDvdVideoPicture->stfbuf, XbmcThreads::SystemClockMillis() - time);
      CLog::Log(LOGDEBUG, ">>>     va:%p,fa:%p,%p, w:%d, h:%d, dw:%d, dh:%d\n",
                vpucopy->vpumem.vir_addr, vpucopy->FrameBusAddr[0], vpucopy->FrameBusAddr[1],
          vpucopy->FrameWidth, vpucopy->FrameHeight, vpucopy->DisplayWidth, vpucopy->DisplayHeight);

//#endif
    }
    else
      pDvdVideoPicture->iFlags |= DVP_FLAG_DROPPED;
  }

  if (drop_state)
    pDvdVideoPicture->iFlags |= DVP_FLAG_DROPPED;

  prev_frame = frame;
  cur_frame = NULL;
  out_condition.notify();
  out_mutex.unlock();

  return true;
}

void CRkStageFrightVideo::Dispose()
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Close\n", CLASSNAME);
#endif

  Frame *frame;

  if (decode_thread && decode_thread->IsRunning())
    decode_thread->StopThread(false);
  decode_thread = NULL;
  in_condition.notify();

  // Give decoder_thread time to process EOS, if stuck on reading
  usleep(50000);

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Cleaning OUT\n");
#endif
  out_mutex.lock();
  if (cur_frame)
  {
    if (cur_frame->medbuf)
      cur_frame->medbuf->release();
    free(cur_frame);
    cur_frame = NULL;
  }
  while (outbuf_queue.size())
  {
    Frame* frame = outbuf_queue.front();
    outbuf_queue.pop_front();
    if (frame->medbuf)
    {
      if (frame->medbuf->range_length() == sizeof(VPU_FRAME))
      {
        VPU_FRAME *vpuframe = (VPU_FRAME *)((long)frame->medbuf->data() + frame->medbuf->range_offset());
        if(vpuframe->vpumem.phy_addr)
        {
          VPUMemLink(&vpuframe->vpumem);
          VPUFreeLinear(&vpuframe->vpumem);
        }
      }
      frame->medbuf->release();
    }
    free(frame);
  }
  while(m_busy_vpu_queue.size())
  {
    VPU_FRAME* vpuframe = m_busy_vpu_queue.front();
    ReleaseVpuFrame(vpuframe);
  }

  out_condition.notify();
  out_mutex.unlock();

  if (prev_frame)
  {
    if (prev_frame->medbuf)
    {
      if (prev_frame->medbuf->range_length() == sizeof(VPU_FRAME))
      {
        VPU_FRAME *vpuframe = (VPU_FRAME *)((long)prev_frame->medbuf->data() + prev_frame->medbuf->range_offset());
        if(vpuframe->vpumem.phy_addr)
        {
          VPUMemLink(&vpuframe->vpumem);
          VPUFreeLinear(&vpuframe->vpumem);
        }
      }
      prev_frame->medbuf->release();
    }
    free(prev_frame);
    prev_frame = NULL;
  }

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Stopping omxcodec\n");
#endif
  if (m_stfdecoder != NULL)
    m_stfdecoder->stop();
  if (m_omxclient)
    m_omxclient->disconnect();

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Cleaning IN(%d)\n", in_queue.size());
#endif
  std::map<int64_t,Frame*>::iterator it;
  while (!m_in_queue.empty())
  {
    it = m_in_queue.begin();
    frame = it->second;
    m_in_queue.erase(it);
    if (frame->medbuf)
      frame->medbuf->release();
    free(frame);
  }
  
  delete m_omxclient;

/*
  if ((p->quirks & QuirkSWRender) == 0)
    m_g_xbmcapp->UninitStagefrightSurface();
*/

  for (int i=0; i<INBUFCOUNT; ++i)
  {
    if (p->inbuf[i])
    {
      p->inbuf[i]->setObserver(NULL);
      p->inbuf[i]->release();
      p->inbuf[i] = NULL;
    }
  }
}

void CRkStageFrightVideo::Reset(void)
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Reset\n", CLASSNAME);
#endif
  Frame* frame;
  in_mutex.lock();
  std::map<int64_t,Frame*>::iterator it;
  while (!m_in_queue.empty())
  {
    it = m_in_queue.begin();
    frame = it->second;
    m_in_queue.erase(it);
    if (frame->medbuf)
      frame->medbuf->release();
    free(frame);
  }
  resetting = true;
  m_framecount = 0;

  in_mutex.unlock();
}

void CRkStageFrightVideo::SetDropState(bool bDrop)
{
  if (bDrop == drop_state)
    return;

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::SetDropState (%d->%d)\n", CLASSNAME,p->drop_state,bDrop);
#endif

  drop_state = bDrop;
}

void CRkStageFrightVideo::SetSpeed(int iSpeed)
{
}

/***************/

void CRkStageFrightVideo::LockVpuFrame(VPU_FRAME *vpuframe)
{
  if (!vpuframe)
    return;

#if defined(DEBUG_VERBOSE)
//  CLog::Log(LOGDEBUG, "Locking %p\n", vpuframe);
#endif
  free_mutex.lock();
  m_busy_vpu_queue.push_back(vpuframe);
  free_mutex.unlock();
}

bool CRkStageFrightVideo::ReleaseVpuFrame(VPU_FRAME *vpuframe)
{
  if (!vpuframe)
    return true;

#if defined(DEBUG_VERBOSE)
//  CLog::Log(LOGDEBUG, "Unlocking %p\n", vpuframe);
#endif
  free_mutex.lock();
  int cnt = 0;
  std::list<VPU_FRAME*>::iterator it = m_busy_vpu_queue.begin();
  std::list<VPU_FRAME*>::iterator itfree;

  for(;it != m_busy_vpu_queue.end(); ++it)
  {
    if ((*it) == vpuframe)
    {
      cnt++;
      if (cnt>1)
        break;
      itfree = it;
    }
  }
  if (!cnt)
  {
    free_mutex.unlock();
    return true;
  }

  m_busy_vpu_queue.erase(itfree);
  if (cnt==1)
  {
#if defined(DEBUG_VERBOSE)
//    CLog::Log(LOGDEBUG, ">>> Deleting\n");
#endif
    if (vpuframe->vpumem.vir_addr)
      VPUFreeLinear(&vpuframe->vpumem);
    free(vpuframe);
    free_mutex.unlock();
    return true;
  }
  free_mutex.unlock();
  return false;
}


void CRkStageFrightVideo::LockBuffer(CDVDVideoCodecStageFrightBuffer *buf)
{
  if (!buf)
    return;

  LockVpuFrame((VPU_FRAME*)buf->context);
}

void CRkStageFrightVideo::ReleaseBuffer(CDVDVideoCodecStageFrightBuffer *buf)
{
  if (!buf)
    return;

  if (ReleaseVpuFrame((VPU_FRAME*)buf->context))
    delete buf;
}
