/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "SessionGsl"

#include "SessionGsl.h"
#include "Stream.h"
#include "QalDefs.h"
#include "Device.h"
#include<algorithm>
#include<vector>
#include<fstream>
#include "PayloadBuilder.h"
#include "ResourceManager.h"

#define GSL_LIB  "libcasa-gsl.so"
#define PLAYBACK 0x1
#define RECORD 0x2
#define BUFFER_EOS 1
#define TAG_STREAM_MFC_SR  STREAM_MFC


typedef int32_t (*gsl_init_t)(struct gsl_init_data *);
typedef int32_t (*gsl_open_t)(const struct gsl_key_vector *,
                              const struct gsl_key_vector *, gsl_handle_t *);
typedef int32_t (*gsl_close_t)(gsl_handle_t);
typedef int32_t (*gsl_set_cal_t)(gsl_handle_t ,const struct gsl_key_vector *, const struct gsl_key_vector *);
typedef int32_t (*gsl_set_config_t)(gsl_handle_t, const struct gsl_key_vector *,
        uint32_t, const struct gsl_key_vector *);
typedef int32_t (*gsl_set_custom_config_t)(gsl_handle_t, const uint8_t *, const size_t);
typedef int32_t (*gsl_get_custom_config_t)(gsl_handle_t, uint8_t *, size_t *);
typedef int32_t (*gsl_ioctl_t)(gsl_handle_t, enum gsl_cmd_id, void *, size_t);
typedef int32_t (*gsl_read_t)(gsl_handle_t, uint32_t, struct gsl_buff *, uint32_t *);
typedef int32_t (*gsl_write_t)(gsl_handle_t, uint32_t, struct gsl_buff *, uint32_t *);
typedef int32_t (*gsl_get_tagged_module_info_t)(const struct gsl_key_vector *,
                              uint32_t, struct gsl_module_id_info **, size_t *);
typedef int32_t (*gsl_get_tagged_custom_config_t)(gsl_handle_t, uint32_t, uint8_t *, size_t);
typedef int32_t (*gsl_register_event_cb_t)(gsl_handle_t, gsl_cb_func_ptr, void *);
typedef void (*gsl_deinit_t)(void);

gsl_ioctl_t gslIoctl;
gsl_get_custom_config_t gslGetCustomConfig;
gsl_set_cal_t gslSetCal;
gsl_set_config_t gslSetConfig;
gsl_set_custom_config_t gslSetCustomConfig;
gsl_get_tagged_module_info_t gslGetTaggedModuleInfo;
gsl_get_tagged_custom_config_t gslGetTaggedCustomConfig;
gsl_register_event_cb_t gslRegisterEventCallBack;
gsl_read_t gslRead;
gsl_open_t gslOpen;
gsl_close_t gslClose;
gsl_write_t gslWrite;
gsl_init_t gslInit;
gsl_deinit_t gslDeinit;

int SessionGsl::seek = 0;
void *SessionGsl::gslLibHandle = NULL;

SessionGsl::SessionGsl()
{
    builder = nullptr;

    builder = new PayloadBuilder();
    if (!builder) {
       QAL_ERR(LOG_TAG,"%s: PayloadloadBuilder creation failed", __func__);
       goto error;
    }

    gkv = new gsl_key_vector;
    if (!gkv) {
        QAL_ERR(LOG_TAG,"%s: Failed to malloc gkv", __func__);
        goto error_1;
    }


    ckv = new gsl_key_vector;
    if (!ckv) {
        QAL_ERR(LOG_TAG,"%s: new ckv failed", __func__);
        goto error_2;
    }


error_2:
    delete gkv;
    gkv = nullptr;

error_1:
    delete builder;
    builder = nullptr;

error:
    return;

}

SessionGsl::SessionGsl(std::shared_ptr<ResourceManager> Rm)
{
    rm = Rm;
    SessionGsl();
}

SessionGsl::~SessionGsl()
{
    delete builder;
    delete gkv;
    delete ckv;

}

int SessionGsl::init(std::string acdbFile)
{
    int ret = 0;
    std::string deltaFileStr = "/data/audio/delta";
    struct gsl_acdb_data_files acdb_files;
    struct gsl_acdb_file delta_file;
    gsl_init_data init_data;

    delta_file.fileNameLen = deltaFileStr.size();
    strlcpy(delta_file.fileName, deltaFileStr.c_str(),
              deltaFileStr.size()+1);

    acdb_files.num_files = 1;
    strlcpy(acdb_files.acdbFiles[0].fileName, acdbFile.c_str(),
              acdbFile.size()+1);

    acdb_files.acdbFiles[0].fileNameLen = acdbFile.size();
    init_data.acdb_files = &acdb_files;
    init_data.acdb_delta_file = &delta_file;
    init_data.acdb_addr = 0x0;
    init_data.max_num_ready_checks = 1;
    init_data.ready_check_interval_ms = 100;

    QAL_DBG(LOG_TAG, "Enter.");
    if(!gslLibHandle) {
        gslLibHandle = dlopen(GSL_LIB, RTLD_NOW);
        if (NULL == gslLibHandle) {
            const char *err_str = dlerror();
            QAL_ERR(LOG_TAG, "DLOPEN failed for %s, %s",
                  GSL_LIB, err_str?err_str:"unknown");
            return -EINVAL;
        }

    }

    /*loading the gsl function symbols*/
    gslInit = (gsl_init_t)dlsym(gslLibHandle, "gsl_init");
    if (!gslInit) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_init", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslSetCal = (gsl_set_cal_t)dlsym(gslLibHandle, "gsl_set_cal");
    if (!gslSetCal) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_set_cal", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslSetConfig = (gsl_set_config_t)dlsym(gslLibHandle, "gsl_set_config");
    if (!gslSetConfig) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_set_config", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslSetCustomConfig = (gsl_set_custom_config_t)dlsym(gslLibHandle,
                          "gsl_set_custom_config");
    if (!gslSetCustomConfig) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_set_custom_config", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslGetCustomConfig = (gsl_get_custom_config_t)dlsym(gslLibHandle,
                          "gsl_get_custom_config");
    if (!gslGetCustomConfig) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_get_custom_config", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslIoctl = (gsl_ioctl_t)dlsym(gslLibHandle, "gsl_ioctl");
    if (!gslIoctl) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_ioctl", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslGetTaggedModuleInfo = (gsl_get_tagged_module_info_t)dlsym(gslLibHandle,
                              "gsl_get_tagged_module_info");
    if (!gslGetTaggedModuleInfo) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_get_tagged_module_info", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslGetTaggedCustomConfig = (gsl_get_tagged_custom_config_t)dlsym(gslLibHandle,
                              "gsl_get_tagged_custom_config");
    if (!gslGetTaggedCustomConfig) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_get_tagged_custom_config", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslRegisterEventCallBack = (gsl_register_event_cb_t)dlsym(gslLibHandle,
                                "gsl_register_event_cb");
    if (!gslRegisterEventCallBack) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_register_event_cb", dlerror());
        return -EINVAL;
    }
    gslRead = (gsl_read_t)dlsym(gslLibHandle, "gsl_read");
    if (!gslRead) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_read", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslOpen = (gsl_open_t)dlsym(gslLibHandle, "gsl_open");
    if (!gslOpen) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_open", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslClose = (gsl_close_t)dlsym(gslLibHandle, "gsl_close");
    if (!gslClose) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_close", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslWrite = (gsl_write_t)dlsym(gslLibHandle, "gsl_write");
    if (!gslWrite) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_write", dlerror());
        ret = -EINVAL;
        goto error;
    }
    gslDeinit = (gsl_deinit_t)dlsym(gslLibHandle, "gsl_deinit");
    if (!gslDeinit) {
        QAL_ERR(LOG_TAG, "dlsym error %s for gsl_deinit", dlerror());
        ret = -EINVAL;
        goto error;
    }
    ret = PayloadBuilder::init();
    if (0 != ret) {
        QAL_ERR(LOG_TAG, "payload builder init failed with err = %d", ret);
        goto error;
    }
    ret = gslInit(&init_data);
    if (0 != ret) {
        QAL_ERR(LOG_TAG, "gsl init failed with err = %d", ret);
        goto error;
    }
    QAL_ERR(LOG_TAG, "Exit. gsl init success with acdb file %s", acdbFile.c_str());
    goto exit;
error:
    dlclose(gslLibHandle);
    gslLibHandle = NULL;
exit:
    return ret;
}

void SessionGsl::deinit()
{
    if(NULL != gslLibHandle){
        gslDeinit();
        dlclose(gslLibHandle);
        gslLibHandle = NULL;
    }
}

void printCustomConfig(const uint8_t* payload, size_t size)
{
    size_t loop;
    uint32_t *temp = (uint32_t *)payload;
    for (loop = 0; loop < size;) {
        QAL_VERBOSE(LOG_TAG,"%0x %0x %0x %0x\n",temp[0+loop],temp[1+loop],
                    temp[2+loop],temp[3+loop]);
        loop = loop + 4;
    }
}

int SessionGsl::open(Stream *s)
{
    int status = 0;
    struct qal_stream_attributes sAttr;

    if (!builder) {
        QAL_ERR(LOG_TAG,"%s: No builder initialized", __func__);
        return -EINVAL;
    }

    status = s->getStreamAttributes(&sAttr);
    if(0 != status) {
        QAL_ERR(LOG_TAG, "getStreamAttributes Failed status %d", status);
        return status;
    }
    QAL_DBG(LOG_TAG, "direction: %d", sAttr.direction);
    gkv = new gsl_key_vector;
    status = builder->populateGkv(s, gkv);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to populate gkv status %d", status);
        goto exit;
    }

    ckv = new gsl_key_vector;
    status = builder->populateCkv(s, ckv, 0, NULL);//0, 0);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to populate ckv status %d", status);
        goto exit;
    }
    status = gslOpen(gkv, ckv, &graphHandle);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to open the graph with status %d", status);
        goto exit;
    }
    setPayloadConfig(s);
    QAL_DBG(LOG_TAG, "Exit. handle:%pK status %d", graphHandle, status);
exit:
     return status;
}

int SessionGsl::setPayloadConfig(Stream *s)
{
    std::vector <int> streamTag;
    std::vector <int> streamPpTag;
    std::vector <int> mixerTag;
    std::vector <int> devicePpTag;
    std::vector <int> deviceTag;
    struct gsl_module_id_info* moduleInfo = NULL;
    size_t moduleInfoSize;
    struct sessionToPayloadParam* sessionData = NULL;
    struct sessionToPayloadParam* deviceData = NULL;
    std::vector<std::shared_ptr<Device>> associatedDevices;
    std::vector<std::shared_ptr<Device>> associatedRecordDevices;
    uint8_t* payload = NULL;
    size_t payloadSize = 0;
    int32_t status = 0;
    int32_t i;
    int32_t dev_id;
    struct qal_stream_attributes sAttr;
    struct qal_device dAttr, devAttr;
    s->getStreamAttributes(&sAttr);
    char epName[128] = {0};
    std::string epname;

    QAL_DBG(LOG_TAG, "Enter.");
    sessionData = (struct sessionToPayloadParam *)calloc(1, sizeof(struct sessionToPayloadParam));
    if (!sessionData) {
        status = -ENOMEM;
        QAL_ERR(LOG_TAG, "sessionData malloc failed status %d", status);
        goto exit;
    }
    deviceData = (struct sessionToPayloadParam *)calloc(1, sizeof(struct sessionToPayloadParam));
    if (!deviceData) {
        status = -ENOMEM;
        QAL_ERR(LOG_TAG, "deviceData malloc failed status %d", status);
        goto free_sessionData;
    }
    status = rm->getStreamTag(streamTag);
    sessionData->direction = sAttr.direction;
    //decision based on stream attributes
    if (sessionData->direction == QAL_AUDIO_INPUT) {
        sessionData->sampleRate = sAttr.in_media_config.sample_rate;
        sessionData->bitWidth = sAttr.in_media_config.bit_width;
        sessionData->numChannel = sAttr.in_media_config.ch_info->channels;
        status = s->getAssociatedDevices(associatedRecordDevices);
        if(0 != status) {
            QAL_ERR(LOG_TAG"%s: getAssociatedDevices Failed \n", __func__);
            return status;
        }
        associatedRecordDevices[0]->getDeviceAtrributes(&devAttr);
        if (devAttr.config.ch_info->channels == sessionData->numChannel) {
            sessionData->native = 1;
        } else {
            sessionData->native = 0;
        }
    } else {
        sessionData->sampleRate = sAttr.out_media_config.sample_rate;
        sessionData->bitWidth = sAttr.out_media_config.bit_width;
        sessionData->numChannel = sAttr.out_media_config.ch_info->channels;
        sessionData->native = 0;
    }
    sessionData->metadata = NULL;
    QAL_DBG(LOG_TAG, "session bit width %d, sample rate %d, and channels %d",
        sessionData->bitWidth, sessionData->sampleRate, sessionData->numChannel);

    for (i=0; i<streamTag.size(); i++) {
        moduleInfo = NULL;
        moduleInfoSize = 0;
        QAL_ERR(LOG_TAG,"tag id %x",streamTag[i]);
        status = gslGetTaggedModuleInfo(gkv, streamTag[i], &moduleInfo, &moduleInfoSize);
        if (0 != status)
            continue;
        else {
            payload = NULL;
            payloadSize = 0;
            builder->payloadStreamConfig(&payload, &payloadSize, moduleInfo,
                                         streamTag[i], sessionData);
            if (!payload) {
                status = -ENOMEM;
                QAL_ERR(LOG_TAG, "failed to get payload status %d", status);
                continue;
            }
            status = gslSetCustomConfig(graphHandle, payload, payloadSize);
            if (0 != status) {
                QAL_ERR(LOG_TAG, "Get custom config failed with status = %d", status);
                //goto free_payload;
            }
        }
    }
    status = s->getAssociatedDevices(associatedDevices);
    if(0 != status) {
        QAL_ERR(LOG_TAG, "getAssociatedDevices Failed status %d", status);
        goto free_payload;
    }

    for (int32_t i=0; i<(associatedDevices.size()); i++) {
        dev_id = associatedDevices[i]->getSndDeviceId();
        rm->getDeviceEpName(dev_id, epname);
        QAL_VERBOSE(LOG_TAG, "epname = %s", epname.c_str());
    }
    status = rm->getDeviceTag(deviceTag);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "failed to get device tag, status %d", status);
        goto free_payload;
    }
    for (i=0; i < deviceTag.size(); i++) {
        moduleInfo = NULL;
        moduleInfoSize = 0;
        deviceData->metadata = NULL;
        status = gslGetTaggedModuleInfo(gkv, deviceTag[i], &moduleInfo, &moduleInfoSize);
        if (0 != status)
             continue;
        else {
            payload = NULL;
            payloadSize = 0;
            if(deviceTag[i] == DEVICE_HW_ENDPOINT_RX) {
                for (int32_t i=0; i<(associatedDevices.size()); i++) {
                    dev_id = associatedDevices[i]->getSndDeviceId();
                    if(dev_id >=QAL_DEVICE_NONE && dev_id <= QAL_DEVICE_OUT_PROXY) {
                        rm->getDeviceEpName(dev_id, epname);
                        associatedDevices[i]->getDeviceAtrributes(&dAttr);
                        deviceData->bitWidth = dAttr.config.bit_width;
                        deviceData->sampleRate = dAttr.config.sample_rate;
                        deviceData->numChannel = dAttr.config.ch_info->channels;
                        QAL_DBG(LOG_TAG, "EP Device bit width %d, sample rate %d,and channels %d",
                                deviceData->bitWidth,
                                deviceData->sampleRate, deviceData->numChannel);
                    } else 
                        continue;
                    sessionData->direction = PLAYBACK;
                }
            } else if (deviceTag[i] == DEVICE_HW_ENDPOINT_TX) {
                for (int32_t i=0; i<(associatedDevices.size()); i++) {
                    dev_id = associatedDevices[i]->getSndDeviceId();
                    if(dev_id >=QAL_DEVICE_IN_HANDSET_MIC && dev_id <= QAL_DEVICE_IN_PROXY) {
                        rm->getDeviceEpName(dev_id, epname);
                        associatedDevices[i]->getDeviceAtrributes(&dAttr);
                        deviceData->bitWidth = dAttr.config.bit_width;
                        deviceData->sampleRate = dAttr.config.sample_rate;
                        deviceData->numChannel = dAttr.config.ch_info->channels;
                        QAL_DBG(LOG_TAG, "EP Device bit width %d, sample rate %d, and channels %d",
                                deviceData->bitWidth,
                                deviceData->sampleRate, deviceData->numChannel);
                    } else
                        continue;
                    sessionData->direction = RECORD;
                }
            } else {
                continue;
            }
            switch (deviceData->sampleRate) {
                case SAMPLINGRATE_8K :
                    setConfig(s,MODULE,MFC_SR_8K);
                    break;
                case SAMPLINGRATE_16K :
                    setConfig(s,MODULE,MFC_SR_16K);
                    break;
                case SAMPLINGRATE_32K :
                    setConfig(s,MODULE,MFC_SR_32K);
                    break;
                case SAMPLINGRATE_44K :
                    setConfig(s,MODULE,MFC_SR_44K);
                    break;
                case SAMPLINGRATE_48K :
                    setConfig(s,MODULE,MFC_SR_48K);
                    break;
                case SAMPLINGRATE_96K :
                    setConfig(s,MODULE,MFC_SR_96K);
                    break;
                case SAMPLINGRATE_192K :
                    setConfig(s,MODULE,MFC_SR_192K);
                    break;
                case SAMPLINGRATE_384K :
                    setConfig(s,MODULE,MFC_SR_384K);
                    break;
                default:
                    QAL_ERR(LOG_TAG, "Invalid sample rate = %d", deviceData->sampleRate);
            }

            if (moduleInfo) {
                builder->payloadDeviceEpConfig(&payload, &payloadSize, moduleInfo,
                                               deviceTag[i], deviceData, epname);
                if (!payload) {
                    status = -ENOMEM;
                    QAL_ERR(LOG_TAG, "failed to get payload status %d", status);
                continue;
                }
                status = gslSetCustomConfig(graphHandle, payload, payloadSize);
                if (0 != status) {
                    QAL_ERR(LOG_TAG, "Get custom config failed with status = %d", status);
                    //goto free_payload;
                }
            }
        }
    }

    for (i=0; i < deviceTag.size(); i++) {
        moduleInfo = NULL;
        moduleInfoSize = 0;
        status = gslGetTaggedModuleInfo(gkv, deviceTag[i], &moduleInfo, &moduleInfoSize);
        deviceData->metadata = NULL;
        if ((status != 0) || (moduleInfo == NULL))
            continue;
        else {
            payload = NULL;
            payloadSize = 0;
            if(deviceTag[i] == DEVICE_HW_ENDPOINT_RX) {
                for (int32_t i=0; i<(associatedDevices.size()); i++) {
                    dev_id = associatedDevices[i]->getSndDeviceId();
                    if(dev_id >=QAL_DEVICE_NONE && dev_id <= QAL_DEVICE_OUT_PROXY) {
                        associatedDevices[i]->getDeviceAtrributes(&dAttr);
                        deviceData->bitWidth = dAttr.config.bit_width;
                        deviceData->sampleRate = dAttr.config.sample_rate;
                        deviceData->numChannel = dAttr.config.ch_info->channels;
                        QAL_DBG(LOG_TAG, "Device bit width %d, sample rate %d, and channels %d",
                                deviceData->bitWidth,
                                deviceData->sampleRate,deviceData->numChannel);
                    } else
                        continue;
                    sessionData->direction = PLAYBACK;
                }
            } else if (deviceTag[i] == DEVICE_HW_ENDPOINT_TX) {
                for (int32_t i=0; i<(associatedDevices.size()); i++) {
                    dev_id = associatedDevices[i]->getSndDeviceId();
                    if(dev_id >=QAL_DEVICE_IN_HANDSET_MIC && dev_id <= QAL_DEVICE_IN_PROXY) {
                       associatedDevices[i]->getDeviceAtrributes(&dAttr);
                       deviceData->bitWidth = dAttr.config.bit_width;
                       deviceData->sampleRate = dAttr.config.sample_rate;
                       deviceData->numChannel = dAttr.config.ch_info->channels;
                       QAL_DBG(LOG_TAG, "Device bit width %d, sample rate %d,and channels %d",
                               deviceData->bitWidth,
                               deviceData->sampleRate,deviceData->numChannel);
                    } else
                       continue;
                    sessionData->direction = RECORD;
                }
            } else
                continue;

            builder->payloadDeviceConfig(&payload, &payloadSize, moduleInfo,
                                         deviceTag[i], deviceData);
            if (!payload) {
                status = -ENOMEM;
                QAL_ERR(LOG_TAG, "failed to get payload status %d", status);
                goto free_moduleInfo;
            }
            status = gslSetCustomConfig(graphHandle, payload, payloadSize);
            if (0 != status) {
                QAL_ERR(LOG_TAG, "Get custom config failed with status = %d",
                        status);
                //goto free_payload;
            }
        }
    }
    QAL_DBG(LOG_TAG, "Exit. status %d", status);
free_payload:
    if (payload)
       free(payload);
free_moduleInfo:
    if (moduleInfo)
       free(moduleInfo);
free_deviceData:
    if (deviceData)
       free(deviceData);
free_sessionData:
    if (sessionData)
       free(sessionData);
exit:
    return status;
}

int SessionGsl::prepare(Stream * s)
{
    int status = 0;
    size_t in_buf_size = 0,in_buf_count = 0;
    size_t out_buf_size = 0,out_buf_count = 0;
    struct qal_stream_attributes sAttr;
    s->getStreamAttributes(&sAttr);

    QAL_DBG(LOG_TAG, "Enter. direction: %d ", sAttr.direction);

    status = gslIoctl(graphHandle, GSL_CMD_PREPARE, NULL, 0);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to prepare the graph, status %d", status);
        goto exit;
    }
    s->getBufInfo(&in_buf_size,&in_buf_count,&out_buf_size,&out_buf_count);
    switch (sAttr.direction) {
    case QAL_AUDIO_INPUT:
            status = readBufferInit(s, in_buf_count, in_buf_size, DATA_MODE_BLOCKING);
            if (0 != status) {
                QAL_ERR(LOG_TAG, "Tx session readBufferInit is failed with status %d",
                        status);
                goto exit;
            }
            break;
    case QAL_AUDIO_OUTPUT:
            status = writeBufferInit(s, out_buf_count, out_buf_size, DATA_MODE_BLOCKING);
            if (0 != status) {
                QAL_ERR(LOG_TAG, "Rx session writeBufferInit is failed with status %d",
                        status);
                goto exit;
            }
            break;
        case QAL_AUDIO_INPUT_OUTPUT:
            break;
        default:
            break;
    }
    QAL_DBG(LOG_TAG, "Exit. status:%d ", status);
exit:
    return status;
}

int SessionGsl::readBufferInit(Stream * s, size_t noOfBuf, size_t bufSize, int flag)
{
    int status = 0;

    QAL_DBG(LOG_TAG, "Enter. bufSize:%d noOfBuf:%d flag:%d", bufSize, noOfBuf, flag);

    infoBuffer = (struct gslCmdGetReadWriteBufInfo*)calloc(1, sizeof(struct gslCmdGetReadWriteBufInfo));
    if (!infoBuffer) {
        status = -ENOMEM;
        QAL_ERR(LOG_TAG, "infoBuffer malloc failed %s status %d", strerror(errno),
                status);
        return status;
    }
    infoBuffer->buff_size = bufSize;
    infoBuffer->num_buffs = noOfBuf;
    infoBuffer->attritubes = flag;
    infoBuffer->start_threshold = 0;
    infoBuffer->stop_threshold = 0;

    size = sizeof(struct gslCmdGetReadWriteBufInfo);
    status = gslIoctl(graphHandle, GSL_CMD_CONFIGURE_READ_PARAMS, infoBuffer, size);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to initialize the read buffer in gsl, status %d");
        goto free_infoBuffer;
    }
    QAL_DBG(LOG_TAG, "Exit. status:%d ", status);
    goto exit;

free_infoBuffer:
    free(infoBuffer);
exit:
    return status;
}

int SessionGsl::writeBufferInit(Stream * s, size_t noOfBuf, size_t bufSize, int flag)
{
    int status = 0;
    struct gslCmdGetReadWriteBufInfo buf;

    QAL_DBG(LOG_TAG, "Enter. bufSize:%d noOfBuf:%d flag:%d", bufSize, noOfBuf, flag);

    infoBuffer = (struct gslCmdGetReadWriteBufInfo*)calloc(1, sizeof(struct gslCmdGetReadWriteBufInfo));
    if (!infoBuffer) {
        status = -ENOMEM;
        QAL_ERR(LOG_TAG, "infoBuffer malloc failed %s status %d", strerror(errno), status);
        return status;
    }
    infoBuffer->buff_size = bufSize;
    infoBuffer->num_buffs = noOfBuf;
    infoBuffer->attritubes = flag;
    infoBuffer->start_threshold = 0;
    infoBuffer->stop_threshold = 0;

    size = sizeof(struct gslCmdGetReadWriteBufInfo);
    status = gslIoctl(graphHandle, GSL_CMD_CONFIGURE_WRITE_PARAMS, infoBuffer,
                      size);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to initialize write buffer in gsl, status %d",
                status);
        goto free_infoBuffer;
    }
    QAL_DBG(LOG_TAG, "Exit. status:%d ", status);
    goto exit;

free_infoBuffer:
    free(infoBuffer);
exit:
    return status;
}

int SessionGsl::close(Stream *s)
{
    int status = 0;
    QAL_DBG(LOG_TAG, "Enter. graphHandle:%pK", graphHandle);

    status = gslClose(graphHandle);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to close the graph, status %d", status);
        goto exit;
    }
    QAL_ERR(LOG_TAG, "gsl_close successful");

    free(gkv->kvp);
    free(gkv);
    gkv = NULL;
    free(ckv->kvp);
    free(ckv);
    ckv = NULL;
    QAL_DBG(LOG_TAG, "Exit. status:%d ", status);
exit:
    return status;
}

int SessionGsl::start(Stream *s)
{
    int status = 0;
    QAL_DBG(LOG_TAG, "Enter. graphHandle:%pK", graphHandle);
    checkAndConfigConcurrency(s);

    checkAndConfigConcurrency(s);
    status = gslIoctl(graphHandle, GSL_CMD_START, payload, size);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to start the graph, status %d", status);
        goto exit;
    }
    QAL_DBG(LOG_TAG, "Exit. status:%d ", status);
exit:
    return status;
}

int SessionGsl::stop(Stream * s)
{
    int status = 0;
    QAL_DBG(LOG_TAG, "Enter. graphHandle:%pK", graphHandle);

    status = gslIoctl(graphHandle, GSL_CMD_STOP, payload, size);
    if(0 != status) {
         QAL_ERR(LOG_TAG, "Failed to stop the graph, status %d", status);
         goto exit;
    }
    QAL_DBG(LOG_TAG, "Exit. status:%d ", status);
exit:
    return status;
}

int SessionGsl::setTKV(Stream * s, configType type, effect_qal_payload_t *payload)
{
    return 0;
}

int SessionGsl::setConfig(Stream *s, configType type, uint32_t tag1,
        uint32_t tag2, uint32_t tag3)
{
    return 0;
}

int SessionGsl::setConfig(Stream *s, configType type, int tag)
{
    int status = 0;
    uint32_t tagsent;
    struct qal_volume_data *voldata = NULL;
    struct gsl_module_id_info *moduleInfo = NULL;
    size_t moduleInfoSize;
    uint8_t* payload = NULL;
    size_t payloadSize = 0;

    QAL_DBG(LOG_TAG, "Enter. graphHandle:%pK type:%d tag:%d", graphHandle, type,
            tag);
    gkv = new gsl_key_vector;
    status = builder->populateGkv(s, gkv);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to populate gkv status %d", status);
        goto exit;
    }

    switch (type) {
    case GRAPH:
        break;
    case MODULE:
        tkv = new gsl_key_vector;
        status = builder->populateTkv(s, tkv, tag, &tagsent);
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Failed to set the tag configuration, status %d",
                    status);
            goto exit;
        }
        QAL_DBG(LOG_TAG, "MODULE: tag:%d tagsent:%x tkv key %x value %x \n", tag,
                tagsent, (tkv->kvp[0].key), (tkv->kvp[0].value));
        //TODO:Remove this hack and payload pause and resume once QACT fixes TKV issue in acdb file
        if (tagsent == TAG_PAUSE) {
            QAL_VERBOSE(LOG_TAG,"Do not call gslSetConfig if tagsent:%x \n", tagsent);
        } else {
            status = gslSetConfig(graphHandle, gkv, tagsent, tkv);
            if (0 != status) {
               QAL_ERR(LOG_TAG, "Failed to set tag data status %d", status);
               goto exit;
           }
        }
        break;
    case CALIBRATION:
//        ckv = new gsl_key_vector;
        status = builder->populateCkv(s, ckv, tag, &voldata); //, graphHandle, this);
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Failed to populate calibration data status %d", status);
            goto exit;
        }
        status = gslGetTaggedModuleInfo(gkv, tag,
                                     &moduleInfo, &moduleInfoSize);
        if (0 != status || !moduleInfo) {
            QAL_ERR(LOG_TAG, "Failed to get tag info %x module size status %d", tag, status);
            goto free_voldata;
        }
        builder->payloadVolume(&payload, &payloadSize, moduleInfo->module_entry[0].module_iid, voldata, tag);
        if (!payload) {
            status = -EINVAL;
            QAL_ERR(LOG_TAG, "failed to get payload status %d", status);
            goto free_moduleinfo;
        }
        QAL_DBG(LOG_TAG, "%x - payload and %d size", payload , payloadSize);
        status = gslSetCustomConfig(graphHandle, payload, payloadSize);
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Get custom config failed with status = %d", status);
        }
		free(payload);
free_moduleinfo:
		free(moduleInfo);
free_voldata:
		free(voldata);
        QAL_DBG(LOG_TAG, "graph handle %x", graphHandle);
        QAL_DBG(LOG_TAG, "ckv key %x value %x\n", (ckv->kvp[0].key),(ckv->kvp[0].value));
        status = gslSetCal(graphHandle, gkv, ckv);
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Failed to set the calibration data status %d", status);
            goto exit;
        }
        break;
    default:
        status = -EINVAL;
        QAL_ERR(LOG_TAG, "invalid type status %d", status);
        goto exit;
    }
    QAL_ERR(LOG_TAG, "Exit. status:%d ", status);

exit:
    return status;
}

int SessionGsl::fileRead(Stream *s, int tag, struct qal_buffer *buf, int * size)
{
    std::fstream fs;
    QAL_DBG(LOG_TAG, "Enter.");

    fs.open ("/data/test.wav", std::fstream::binary | std::fstream::in | std::fstream::app);
    QAL_VERBOSE(LOG_TAG, "file open success");
    char * buff = static_cast<char *>(buf->buffer);
    if(seek != 0) {
       fs.seekp(seek);
    }
    fs.read (buff,buf->size);
    seek += buf->size;
    QAL_VERBOSE(LOG_TAG, "file read success");
    fs.close();
    QAL_VERBOSE(LOG_TAG, "file close success");
    *size = (int)(buf->size);
    QAL_DBG(LOG_TAG, "Exit. size: %d", *size);
    return 0;
}

int SessionGsl::read(Stream *s, int tag, struct qal_buffer *buf, int * size)
{
    int status = 0, bytesRead = 0, bytesToRead = 0, offset = 0;
    QAL_DBG(LOG_TAG, "Enter. graphHandle:%pK buf:%pK tag:%d", graphHandle, buf, tag);
    if (!buf || !s) {
        status = -EINVAL;
        QAL_ERR(LOG_TAG, "Invalid stream or buffer, status %d", status);
        goto exit;
    }

    struct gsl_buff gslBuff;
    QAL_DBG(LOG_TAG, "bufsize:%d bufNo:%d", infoBuffer->buff_size, infoBuffer->num_buffs);

    while (1) {
        offset = bytesRead + buf->offset;
        bytesToRead = buf->size - offset;
        if (!bytesToRead)
            break;
        gslBuff.flags = 0;
        if ((bytesToRead / infoBuffer->buff_size) >= 1)
            gslBuff.size = infoBuffer->buff_size;
        else
            gslBuff.size = bytesToRead;


        uint32_t sizeRead;
        void *data = buf->buffer;
        data = static_cast<char*>(data) + offset;
        gslBuff.addr = static_cast<uint8_t*>(data);
        status = gslRead(graphHandle, tag, &gslBuff, &sizeRead);
        if ((0 != status) || (sizeRead == 0)) {
            QAL_ERR(LOG_TAG, "Failed to read data from gsl %d bytes read %d", status,
                    sizeRead);
            break;
        }

        if (!bytesRead && buf->ts) {
            buf->ts->tv_sec = gslBuff.timestamp / 1000000;
            buf->ts->tv_nsec = (gslBuff.timestamp - buf->ts->tv_sec * 1000000) * 1000;
            QAL_VERBOSE(LOG_TAG, "Timestamp %llu, tv_sec = %ld, tv_nsec = %ld",
                        gslBuff.timestamp, buf->ts->tv_sec, buf->ts->tv_nsec);
        }
        QAL_DBG(LOG_TAG, "bytes read %d and  sizeRead %d", bytesRead, sizeRead);
        bytesRead += sizeRead;
    }
    QAL_DBG(LOG_TAG, "Exit. bytesRead:%d status:%d ", bytesRead, status);
exit:
    *size = bytesRead;
    return status;
}

int SessionGsl::fileWrite(Stream *s, int tag, struct qal_buffer *buf, int * size, int flag)
{
    std::fstream fs;
    QAL_DBG(LOG_TAG, "Enter.");

    fs.open ("/data/test.wav", std::fstream::binary | std::fstream::out | std::fstream::app);
    QAL_DBG(LOG_TAG, "file open success");
    char * buff=static_cast<char *>(buf->buffer);
    fs.write (buff,buf->size);
    QAL_VERBOSE(LOG_TAG, "file write success");
    fs.close();
    QAL_VERBOSE(LOG_TAG, "file close success");
    *size = (int)(buf->size);
    QAL_DBG(LOG_TAG,"iExit. size: %d", *size);
    return 0;
}

int SessionGsl::write(Stream *s, int tag, struct qal_buffer *buf, int * size, int flag)
{
    int status = 0, bytesWritten = 0, bytesRemaining = 0, offset = 0;
    uint32_t sizeWritten = 0;
    QAL_DBG(LOG_TAG, "Enter. graphHandle:%pK buf:%pK tag:%d flag:%d", graphHandle,
            buf, tag, flag);

    void *data = nullptr;
    struct gsl_buff gslBuff;
    gslBuff.timestamp = (uint64_t) buf->ts;

    bytesRemaining = buf->size;

    while ((bytesRemaining / infoBuffer->buff_size) > 1) {
        gslBuff.flags = 0;

        offset = bytesWritten + buf->offset;
        gslBuff.size = infoBuffer->buff_size;
        data = buf->buffer;
        data = static_cast<char *>(data) + offset;

        gslBuff.addr = static_cast<uint8_t *>(data);
        sizeWritten = 0;  //initialize 0
        status = gslWrite(graphHandle, tag, &gslBuff, &sizeWritten);
        if (0 != status) {
            QAL_ERR(LOG_TAG, "Failed to write the data to gsl, status %d", status);
            goto exit;
        }
        bytesWritten += sizeWritten;
        bytesRemaining -= sizeWritten;
    }

    if (BUFFER_EOS == flag)
        gslBuff.flags = BUFF_FLAG_EOS;
    else
        gslBuff.flags = 0;
    offset = bytesWritten + buf->offset;
    gslBuff.size = bytesRemaining;
    data = buf->buffer;
    data = static_cast<char *>(data) + offset;
    gslBuff.addr = static_cast<uint8_t *>(data);
    sizeWritten = 0;  //0
    status = gslWrite(graphHandle, tag, &gslBuff, &sizeWritten);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to write the data to gsl, status %d", status);
        goto exit;
    }
    bytesWritten += sizeWritten;
    *size = bytesWritten;
    QAL_DBG(LOG_TAG, "Exit. bytesWritten:%d status:%d ", bytesWritten, status);
exit:
    return 0;
}

int SessionGsl::getParameters(Stream *s, int tagId, uint32_t param_id, void **payload)
{
    int status = 0;
    uint8_t *data = NULL;
    uint8_t *config = NULL;
    size_t moduleInfoSize;
    size_t payloadSize = 0;
    struct apm_module_param_data_t *header = NULL;
    struct gsl_module_id_info *moduleInfo = NULL;
    QAL_DBG(LOG_TAG, "Enter.");

    status = gslGetTaggedModuleInfo(gkv, tagId,
                            &moduleInfo, &moduleInfoSize);
    if (0 != status || !moduleInfo) {
        QAL_ERR(LOG_TAG, "Failed to get tag info %x, status %d", tagId, status);
        goto exit;
    }

    switch (param_id) {
        case QAL_PARAM_ID_DIRECTION_OF_ARRIVAL:
        {
            payloadSize = sizeof(struct apm_module_param_data_t) +
                          sizeof(ffv_doa_tracking_monitor_t);
            data = (uint8_t*)calloc(1, payloadSize);
            config = (uint8_t *)calloc(1, sizeof(ffv_doa_tracking_monitor_t));
            if (!data || !config) {
                status = ENOMEM;
                QAL_ERR(LOG_TAG, "Failed to allocate memory for DOA payload");
                goto exit;
            }

            header = (struct apm_module_param_data_t *)data;
            header->module_instance_id = moduleInfo->module_entry[0].module_iid;
            header->param_id = PARAM_ID_FFV_DOA_TRACKING_MONITOR;
            header->error_code = 0x0;
            header->param_size = payloadSize - sizeof(struct apm_module_param_data_t);

            status = gslGetTaggedCustomConfig(graphHandle, tagId, data, payloadSize);
            if (status) {
                QAL_ERR(LOG_TAG, "Failed to get DOA info from gsl, status = %d", status);
                goto exit;
            }

            casa_osal_memcpy(config, sizeof(ffv_doa_tracking_monitor_t),
                             data + sizeof(struct apm_module_param_data_t),
                             sizeof(ffv_doa_tracking_monitor_t));
            *payload = (void *)config;
            break;
        }
        default:
            status = EINVAL;
            QAL_ERR(LOG_TAG, "Unsupported param id %u status %d", param_id, status);
            goto exit;
    }

exit:
    if (data)
        free(data);
    QAL_DBG(LOG_TAG, "Exit. status %d", status);
    return status;
}

int SessionGsl::setParameters(Stream *s, int tagId, uint32_t param_id, void *payload)
{
    int status = 0;
    size_t moduleInfoSize;
    struct gsl_module_id_info *moduleInfo = NULL;
    uint8_t *param = NULL;
    size_t paramSize;
    QAL_DBG(LOG_TAG, "Enter.");

    PayloadBuilder *builder = new PayloadBuilder();
    if (!builder) {
        status = -ENOMEM;
        QAL_ERR(LOG_TAG, "Failed to get builder status %d", status);
        goto exit;
    }

    status = gslGetTaggedModuleInfo(gkv, tagId,
                            &moduleInfo, &moduleInfoSize);
    if (0 != status || !moduleInfo) {
        QAL_ERR(LOG_TAG, "Failed to get tag info %x, status %d", tagId, status);
        goto free_builder;
    }

    switch (param_id) {
        case PARAM_ID_DETECTION_ENGINE_SOUND_MODEL:
        {
            struct qal_st_sound_model *pSoundModel = NULL;
            pSoundModel = (struct qal_st_sound_model *)payload;
            builder->payloadSVASoundModel(&param, &paramSize,
                                          moduleInfo->module_entry[0].module_iid,
                                          pSoundModel);
           break;
        }
        case PARAM_ID_DETECTION_ENGINE_CONFIG_VOICE_WAKEUP:
        {
            struct detection_engine_config_voice_wakeup *pWakeUpConfig = NULL;
            pWakeUpConfig = (struct detection_engine_config_voice_wakeup *)payload;
            builder->payloadSVAWakeUpConfig(&param, &paramSize,
                                            moduleInfo->module_entry[0].module_iid,
                                            pWakeUpConfig);
            break;
        }
        case PARAM_ID_DETECTION_ENGINE_GENERIC_EVENT_CFG:
        {
            // register callback in gsl
            status = gslRegisterEventCallBack(graphHandle, stCallBack, (void*)s);
            if (0 != status) {
                QAL_ERR(LOG_TAG, "Failed to register recognition callback in gsl status %d",
                        status);
                goto free_moduleInfo;
            }

            // Register custom config
            struct gsl_cmd_register_custom_event reg_ev_payload;
            reg_ev_payload.event_id = EVENT_ID_DETECTION_ENGINE_GENERIC_INFO;
            reg_ev_payload.module_instance_id = moduleInfo->module_entry[0].module_iid;
            reg_ev_payload.event_config_payload_size = 0;
            reg_ev_payload.is_register = 1;
            status = gslIoctl(graphHandle, GSL_CMD_REGISTER_CUSTOM_EVENT, &reg_ev_payload,
                              sizeof(reg_ev_payload));
            if (0 != status) {
                QAL_ERR(LOG_TAG, "Failed to register custom event with status = %d\n", status);
                goto free_moduleInfo;
            }

            struct detection_engine_generic_event_cfg *pEventConfig = NULL;
            pEventConfig = (struct detection_engine_generic_event_cfg *)payload;
            builder->payloadSVAEventConfig(&param, &paramSize,
                                           moduleInfo->module_entry[0].module_iid,
                                           pEventConfig);
            break;
        }
        case PARAM_ID_VOICE_WAKEUP_BUFFERING_CONFIG:
        {
            struct detection_engine_voice_wakeup_buffer_config *pWakeUpBufConfig =
                                                                           NULL;
            pWakeUpBufConfig = (struct detection_engine_voice_wakeup_buffer_config *)payload;
            builder->payloadSVAWakeUpBufferConfig(&param, &paramSize,
                                                  moduleInfo->module_entry[0].module_iid,
                                                  pWakeUpBufConfig);
            break;
        }
        case PARAM_ID_AUDIO_DAM_DOWNSTREAM_SETUP_DURATION:
        {
            struct audio_dam_downstream_setup_duration *pSetupDuration = NULL;
            pSetupDuration = (struct audio_dam_downstream_setup_duration *)payload;
            builder->payloadSVAStreamSetupDuration(&param, &paramSize,
                                                   moduleInfo->module_entry[0].module_iid,
                                                   pSetupDuration);
            break;
        }
        case PARAM_ID_DETECTION_ENGINE_RESET:
        {
            // Unregister custom config
            struct gsl_cmd_register_custom_event reg_ev_payload;
            reg_ev_payload.event_id = EVENT_ID_DETECTION_ENGINE_GENERIC_INFO;
            reg_ev_payload.module_instance_id = moduleInfo->module_entry[0].module_iid;
            reg_ev_payload.event_config_payload_size = 0;
            reg_ev_payload.is_register = 0;
            status = gslIoctl(graphHandle, GSL_CMD_REGISTER_CUSTOM_EVENT, &reg_ev_payload,
                              sizeof(reg_ev_payload));
            if (0 != status) {
                QAL_ERR(LOG_TAG, "Failed to register custom event with status = %d\n", status);
                goto free_moduleInfo;
            }

            builder->payloadSVAEngineReset(&param, &paramSize,
                                           moduleInfo->module_entry[0].module_iid);
            break;
        }
        default:
            status = -EINVAL;
            QAL_ERR(LOG_TAG, "Unsupported param id %u status %d", param_id, status);
            goto free_moduleInfo;
    }

    if (!param) {
        status = -ENOMEM;
        QAL_ERR(LOG_TAG, "failed to get payload status %d", status);
        goto free_moduleInfo;
    }
    QAL_DBG(LOG_TAG, "%x - payload and %d size", param , paramSize);

    status = gslSetCustomConfig(graphHandle, param, paramSize);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Set custom config failed with status = %d", status);
        goto free_payload;
    }
    QAL_DBG(LOG_TAG, "Exit. status %d", status);

free_payload :
    free(param);
free_moduleInfo :
    free(moduleInfo);
free_builder :
    delete builder;
exit:
    return status;
}

void SessionGsl::stCallBack(struct gsl_event_cb_params *event_params, void *client_data)
{
    QAL_DBG(LOG_TAG, "stCallBack invoked");
    qal_stream_callback callBack;
    Stream *s = (Stream *)client_data;
    s->getCallBack(&callBack);
    QAL_DBG(LOG_TAG, "CallBack acquired %pK", callBack);
    qal_stream_handle_t *stream_handle = static_cast<void*>(s);
    uint32_t event_id = event_params->event_id;
    uint32_t *event_data = (uint32_t *)(event_params->event_payload);
    callBack(stream_handle, event_id, event_data, NULL);
}

void SessionGsl::checkAndConfigConcurrency(Stream *s)
{
    int32_t status = 0;
    std::shared_ptr<Device> rxDevice = nullptr;
    std::shared_ptr<Device> txDevice = nullptr;
    std::vector <std::pair<int,int>> keyVector;
    struct gsl_cmd_graph_select device_graph = {{0, nullptr}, {0, nullptr}};
    struct qal_stream_attributes sAttr;
    std::vector <Stream *> activeStreams;
    qal_stream_type_t txStreamType = QAL_STREAM_LOW_LATENCY;
    std::vector <std::shared_ptr<Device>> activeDevices;
    std::vector <std::shared_ptr<Device>> deviceList;

    // get stream attributes
    status = s->getStreamAttributes(&sAttr);
    if (status != 0) {
        QAL_ERR(LOG_TAG,"stream get attributes failed");
        return;
    }

    // get associated device list
    status = s->getAssociatedDevices(deviceList);
    if (0 != status) {
        QAL_ERR(LOG_TAG, "Failed to get associated device, status %d", status);
        return;
    }

    // get all active devices from rm and
    // determine Rx and Tx for concurrency usecase
    rm->getActiveDevices(activeDevices);
    for (int i = 0; i < activeDevices.size(); i++) {
        int deviceId = activeDevices[i]->getSndDeviceId();
        if (deviceId == QAL_DEVICE_OUT_SPEAKER &&
            sAttr.direction == QAL_AUDIO_INPUT) {
            rxDevice = activeDevices[i];
            for (int j = 0; j < deviceList.size(); j++) {
                std::shared_ptr<Device> dev = deviceList[j];
                if (dev->getSndDeviceId() >= QAL_DEVICE_IN_HANDSET_MIC &&
                    dev->getSndDeviceId() <= QAL_DEVICE_IN_TRI_MIC)
                    txDevice = dev;
            }
        }
        //TBD we SHOULD not be checking for individual devices like this. We need to make this more flexible

        if (deviceId >= QAL_DEVICE_IN_HANDSET_MIC &&
            deviceId <= QAL_DEVICE_IN_TRI_MIC &&
            sAttr.direction == QAL_AUDIO_OUTPUT) {
            txDevice = activeDevices[i];
            for (int j = 0; j < deviceList.size(); j++) {
                std::shared_ptr<Device> dev = deviceList[j];
                if (dev->getSndDeviceId() == QAL_DEVICE_OUT_SPEAKER) {
                    rxDevice = dev;
                    break;
                }
            }
        }
    }

    if (!rxDevice || !txDevice) {
        QAL_ERR(LOG_TAG, "No need to handle for concurrency");
        return;
    }

    QAL_DBG(LOG_TAG, "rx device %d, tx device %d", rxDevice->getSndDeviceId(), txDevice->getSndDeviceId());
    // determine concurrency usecase
    for (int i = 0; i < deviceList.size(); i++) {
        std::shared_ptr<Device> dev = deviceList[i];
        if (dev == rxDevice) {
            rm->getActiveStream(txDevice, activeStreams);
            for (int j = 0; j < activeStreams.size(); j++) {
                activeStreams[j]->getStreamType(&txStreamType);
            }
        }
        else if (dev == txDevice)
            s->getStreamType(&txStreamType);
        else {
            QAL_ERR(LOG_TAG, "Concurrency usecase exists, not related to current stream");
            return;
        }
    }

    QAL_DBG(LOG_TAG, "tx stream type = %d", txStreamType);
    // TODO: use table to map types/devices to key values
    if (txStreamType == QAL_STREAM_VOICE_UI) {
        keyVector.push_back(std::make_pair(STREAMTX, VOICE_UI));
        keyVector.push_back(std::make_pair(DEVICEPP_TX,DEVICEPP_TX_VOICE_UI_FLUENCE_FFECNS));
    } else if (txStreamType == QAL_STREAM_VOIP_TX) {
        keyVector.push_back(std::make_pair(STREAMTX, VOIP_TX_RECORD));
        keyVector.push_back(std::make_pair(DEVICEPP_TX,DEVICEPP_TX_VOIP_FLUENCE_PRO));
    }
    else if (txStreamType == QAL_STREAM_LOW_LATENCY && sAttr.direction == QAL_AUDIO_INPUT
             && (sAttr.in_media_config.ch_info->channels >= 3)) {
        keyVector.push_back(std::make_pair(STREAMTX, PCM_RECORD));
        keyVector.push_back(std::make_pair(DEVICEPP_TX,DEVICEPP_TX_AUDIO_FLUENCE_PRO));
    }
    else if (txStreamType == QAL_STREAM_LOW_LATENCY && sAttr.direction == QAL_AUDIO_INPUT
              && (sAttr.in_media_config.ch_info->channels == 1)) {
        keyVector.push_back(std::make_pair(STREAMTX, PCM_RECORD));
        keyVector.push_back(std::make_pair(DEVICEPP_TX,DEVICEPP_TX_HFP_SINK_FLUENCE_SMECNS));
    }
    else
        // TODO: handle for other concurrency usecases also
        return;

    if (txDevice->getSndDeviceId() >= QAL_DEVICE_IN_HANDSET_MIC ||
        txDevice->getSndDeviceId() <= QAL_DEVICE_IN_TRI_MIC)
        keyVector.push_back(std::make_pair(DEVICETX, HANDSETMIC));

    if (rxDevice->getSndDeviceId() == QAL_DEVICE_OUT_SPEAKER)
        keyVector.push_back(std::make_pair(DEVICERX, SPEAKER));
    device_graph.graph_key_vector.num_kvps = keyVector.size();
    device_graph.graph_key_vector.kvp = new struct gsl_key_value_pair[keyVector.size()];
    for(int32_t i = 0; i < (keyVector.size()); i++) {
        device_graph.graph_key_vector.kvp[i].key = keyVector[i].first;
        device_graph.graph_key_vector.kvp[i].value = keyVector[i].second;
        QAL_VERBOSE(LOG_TAG,"kv[%d] key %x value %x", i, (device_graph.graph_key_vector.kvp[i].key),(device_graph.graph_key_vector.kvp[i].value));
    }
    status = gslIoctl(graphHandle, GSL_CMD_ADD_GRAPH, &device_graph,
                      sizeof(device_graph));
    if (0 != status)
        QAL_ERR(LOG_TAG, "Failed to add graph status %d", status);
    delete(device_graph.graph_key_vector.kvp);
}

int SessionGsl::getTimestamp(struct qal_session_time* /*stime*/)
{
   return 0;
}

int SessionGsl::registerCallBack(session_callback /*cb*/, void* /*cookie*/)
{
    return 0;
}

int SessionGsl::drain(qal_drain_type_t /*type*/)
{
    return 0;
}

int SessionGsl::connectSessionDevice(Stream* /*streamHandle*/, qal_stream_type_t /*streamType*/,
        std::shared_ptr<Device> /*deviceToConnect*/)
{
    return 0;
}

int SessionGsl::disconnectSessionDevice(Stream* /*streamHandle*/, qal_stream_type_t /*streamType*/,
        std::shared_ptr<Device> /*deviceToDisconnect*/)
{
    return 0;
}

