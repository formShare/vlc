/*****************************************************************************
 * auhal.c: AUHAL and Coreaudio output plugin
 *****************************************************************************
 * Copyright (C) 2005 - 2017 VLC authors and VideoLAN
 *
 * Authors: Derk-Jan Hartman <hartman at videolan dot org>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *          David Fuhrmann <david dot fuhrmann at googlemail dot com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#pragma mark includes

#ifdef HAVE_CONFIG_H
# import "config.h"
#endif

#import <vlc_atomic.h>
#import <vlc_common.h>
#import <vlc_plugin.h>
#import <vlc_dialog.h>                      // vlc_dialog_display_error
#import <vlc_aout.h>                        // aout_*

#import <AudioUnit/AudioUnit.h>             // AudioUnit
#import <CoreAudio/CoreAudio.h>             // AudioDeviceID
#import <AudioToolbox/AudioFormat.h>        // AudioFormatGetProperty
#import <CoreServices/CoreServices.h>

#import "TPCircularBuffer.h"

#pragma mark -
#pragma mark local prototypes & module descriptor


#define AOUT_VOLUME_DEFAULT             256
#define AOUT_VOLUME_MAX                 512

#define VOLUME_TEXT N_("Audio volume")
#define VOLUME_LONGTEXT VOLUME_TEXT

#define DEVICE_TEXT N_("Last audio device")
#define DEVICE_LONGTEXT DEVICE_TEXT

static int      Open                    (vlc_object_t *);
static void     Close                   (vlc_object_t *);

vlc_module_begin ()
    set_shortname("auhal")
    set_description(N_("HAL AudioUnit output"))
    set_capability("audio output", 101)
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AOUT)
    set_callbacks(Open, Close)
    add_integer("auhal-volume", AOUT_VOLUME_DEFAULT,
                VOLUME_TEXT, VOLUME_LONGTEXT, true)
    change_integer_range(0, AOUT_VOLUME_MAX)
    add_string("auhal-audio-device", "", DEVICE_TEXT, DEVICE_LONGTEXT, true)
    add_obsolete_integer("macosx-audio-device") /* since 2.1.0 */
vlc_module_end ()

#pragma mark -
#pragma mark private declarations

#define STREAM_FORMAT_MSG(pre, sfm) \
    pre "[%f][%4.4s][%u][%u][%u][%u][%u][%u]", \
    sfm.mSampleRate, (char *)&sfm.mFormatID, \
    (unsigned int)sfm.mFormatFlags, (unsigned int)sfm.mBytesPerPacket, \
    (unsigned int)sfm.mFramesPerPacket, (unsigned int)sfm.mBytesPerFrame, \
    (unsigned int)sfm.mChannelsPerFrame, (unsigned int)sfm.mBitsPerChannel

#define AOUT_VAR_SPDIF_FLAG 0xf00000

#define AUDIO_BUFFER_SIZE_IN_SECONDS ((AOUT_MAX_ADVANCE_TIME + CLOCK_FREQ) / CLOCK_FREQ)

/*****************************************************************************
 * aout_sys_t: private audio output method descriptor
 *****************************************************************************
 * This structure is part of the audio output thread descriptor.
 * It describes the CoreAudio specific properties of an output thread.
 *****************************************************************************/
struct aout_sys_t
{
    /* DeviceID of the selected device */
    AudioObjectID               i_selected_dev;
    /* DeviceID of device which will be selected on start */
    AudioObjectID               i_new_selected_dev;
    bool                        b_selected_dev_is_digital;
    /* true if the user selected the default audio device (id 0) */
    bool                        b_selected_dev_is_default;

    /* DeviceID of current device */
    AudioDeviceIOProcID         i_procID;
    /* Are we running in digital mode? */
    bool                        b_digital;

    uint8_t                     chans_to_reorder;
    uint8_t                     chan_table[AOUT_CHAN_MAX];

    /* circular buffer to swap the audio data */
    TPCircularBuffer            circular_buffer;
    atomic_uint                 i_underrun_size;

    /* AUHAL specific */
    AudioComponent              au_component;
    AudioUnit                   au_unit;

    /* CoreAudio SPDIF mode specific */
    /* Keep the pid of our hog status */
    pid_t                       i_hog_pid;
    /* The StreamID that has a cac3 streamformat */
    AudioStreamID               i_stream_id;
    /* The index of i_stream_id in an AudioBufferList */
    int                         i_stream_index;
    /* The original format of the stream */
    AudioStreamBasicDescription sfmt_revert;
    /* Whether we need to revert the stream format */
    bool                        b_revert;
    /* Whether we need to set the mixing mode back */
    bool                        b_changed_mixing;

    /* media sample rate */
    int                         i_rate;
    unsigned int                i_bytes_per_frame;
    unsigned int                i_frame_length;

    CFArrayRef                  device_list;
    /* protects access to device_list */
    vlc_mutex_t                 device_list_lock;

    /* Synchronizes access to i_selected_dev. This is only needed between VLCs
     * audio thread and the core audio callback thread. The value is only
     * changed in Start, further access to this variable within the audio
     * thread (start, stop, close) needs no protection. */
    vlc_mutex_t                 selected_device_lock;

    float                       f_volume;
    bool                        b_mute;
    atomic_bool                 b_paused;

    bool                        b_ignore_streams_changed_callback;

    /* The time the device needs to process the data. In samples. */
    UInt32                      i_device_latency;
};

#pragma mark -
#pragma mark helpers

static int
AoGetProperty(audio_output_t *p_aout, AudioObjectID id,
              const AudioObjectPropertyAddress *p_address, size_t i_elm_size,
              ssize_t i_nb_expected_elms, size_t *p_nb_elms, void **pp_out_data,
              void *p_allocated_out_data)
{
    assert(i_elm_size > 0);

    /* Get data size */
    UInt32 i_out_size;
    OSStatus err = AudioObjectGetPropertyDataSize(id, p_address, 0, NULL,
                                                  &i_out_size);
    if (err != noErr)
    {
        msg_Err(p_aout, "AudioObjectGetPropertyDataSize failed, device id: %i, "
                "prop: [%4.4s], err: [%4.4s]", id, (const char *) &p_address[0],
                (const char *)&err);
        return VLC_EGENERIC;
    }

    size_t i_nb_elms = i_out_size / i_elm_size;
    if (p_nb_elms != NULL)
        *p_nb_elms = i_nb_elms;
    /* Check if we get the expected number of elements */
    if (i_nb_expected_elms != -1 && (size_t)i_nb_expected_elms != i_nb_elms)
    {
        msg_Err(p_aout, "AoGetProperty error: expected elements don't match");
        return VLC_EGENERIC;
    }

    if (pp_out_data == NULL && p_allocated_out_data == NULL)
        return VLC_SUCCESS;

    if (i_out_size == 0)
    {
        if (pp_out_data != NULL)
            *pp_out_data = NULL;
        return VLC_SUCCESS;
    }

    /* Alloc data or use pre-allocated one */
    void *p_out_data;
    if (pp_out_data != NULL)
    {
        assert(p_allocated_out_data == NULL);

        *pp_out_data = malloc(i_out_size);
        if (*pp_out_data == NULL)
            return VLC_ENOMEM;
        p_out_data = *pp_out_data;
    }
    else
    {
        assert(p_allocated_out_data != NULL);
        p_out_data = p_allocated_out_data;
    }

    /* Fill data */
    err = AudioObjectGetPropertyData(id, p_address, 0, NULL, &i_out_size,
                                     p_out_data);
    if (err != noErr)
    {
        msg_Err(p_aout, "AudioObjectGetPropertyData failed, device id: %i, "
                "prop: [%4.4s], err: [%4.4s]", id, (const char *) &p_address[0],
                (const char *)&err);

        if (pp_out_data != NULL)
            free(*pp_out_data);
        return VLC_EGENERIC;
    }
    assert(p_nb_elms == NULL || *p_nb_elms == (i_out_size / i_elm_size));
    return VLC_SUCCESS;
}

/* Get Audio Object Property data: pp_out_data will be allocated by this MACRO
 * and need to be freed in case of success. */
#define AO_GETPROP(id, type, p_out_nb_elms, pp_out_data, a1, a2) \
    AoGetProperty(p_aout, (id), \
                  &(AudioObjectPropertyAddress) {(a1), (a2), 0}, sizeof(type), \
                  -1, (p_out_nb_elms), (void **)(pp_out_data), NULL)

/* Get 1 Audio Object Property data: pre-allocated by the caller */
#define AO_GET1PROP(id, type, p_out_data, a1, a2) \
    AoGetProperty(p_aout, (id), \
                  &(AudioObjectPropertyAddress) {(a1), (a2), 0}, sizeof(type), \
                  1, NULL, NULL, (p_out_data))

static bool
AoIsPropertySettable(audio_output_t *p_aout, AudioObjectID id,
                     const AudioObjectPropertyAddress *p_address)
{
    Boolean b_settable;
    OSStatus err = AudioObjectIsPropertySettable(id, p_address, &b_settable);
    if (err != noErr)
    {
        msg_Warn(p_aout, "AudioObjectIsPropertySettable failed, device id: %i, "
                 "prop: [%4.4s], err: [%4.4s]", id, (const char *)&p_address[0],
                 (const char *)&err);
        return false;
    }
    return b_settable;
}

#define AO_ISPROPSETTABLE(id, a1, a2) \
    AoIsPropertySettable(p_aout, (id), \
                         &(AudioObjectPropertyAddress) { (a1), (a2), 0})

#define AO_HASPROP(id, a1, a2) \
    AudioObjectHasProperty((id), &(AudioObjectPropertyAddress) { (a1), (a2), 0})

static int
AoSetProperty(audio_output_t *p_aout, AudioObjectID id,
              const AudioObjectPropertyAddress *p_address, size_t i_data,
              const void *p_data)
{
    OSStatus err =
        AudioObjectSetPropertyData(id, p_address, 0, NULL, i_data, p_data);

    if (err != noErr)
    {
        msg_Err(p_aout, "AudioObjectSetPropertyData failed, device id: %i, "
                 "prop: [%4.4s], err: [%4.4s]", id, (const char *)&p_address[0],
                 (const char *)&err);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

#define AO_SETPROP(id, i_data, p_data, a1, a2) \
    AoSetProperty(p_aout, (id), \
                  &(AudioObjectPropertyAddress) { (a1), (a2), 0}, \
                  (i_data), (p_data));

static int
AoUpdateListener(audio_output_t *p_aout, bool add, AudioObjectID id,
                 const AudioObjectPropertyAddress *p_address,
                 AudioObjectPropertyListenerProc listener, void *data)
{
    OSStatus err = add ?
        AudioObjectAddPropertyListener(id, p_address, listener, data) :
        AudioObjectRemovePropertyListener(id, p_address, listener, data);

    if (err != noErr)
    {
        msg_Err(p_aout, "AudioObject%sPropertyListener failed, device id %i, "
                "prop: [%4.4s], err: [%4.4s]", add ? "Add" : "Remove", id,
                (const char *)&p_address[0], (const char *)&err);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

#define AO_UPDATELISTENER(id, add, listener, data, a1, a2) \
    AoUpdateListener(p_aout, add, (id), \
                  &(AudioObjectPropertyAddress) { (a1), (a2), 0}, \
                  (listener), (data))

#pragma mark -
#pragma mark Stream / Hardware Listeners

static OSStatus
StreamsChangedListener(AudioObjectID, UInt32,
                       const AudioObjectPropertyAddress [], void *);

static int
ManageAudioStreamsCallback(audio_output_t *p_aout, AudioDeviceID i_dev_id,
                           bool b_register)
{
    /* Retrieve all the output streams */
    size_t i_streams;
    AudioStreamID *p_streams;
    int ret = AO_GETPROP(i_dev_id, AudioStreamID, &i_streams, &p_streams,
                          kAudioDevicePropertyStreams,
                          kAudioObjectPropertyScopeOutput);
    if (ret != VLC_SUCCESS)
        return ret;

    for (size_t i = 0; i < i_streams; i++)
    {
        /* get notified when physical formats change */
        AO_UPDATELISTENER(p_streams[i], b_register, StreamsChangedListener,
                          p_aout, kAudioStreamPropertyAvailablePhysicalFormats,
                          kAudioObjectPropertyScopeGlobal);
    }

    free(p_streams);
    return VLC_SUCCESS;
}

/*
 * AudioStreamSupportsDigital: Checks if audio stream is compatible with raw
 * bitstreams
 */
static bool
AudioStreamSupportsDigital(audio_output_t *p_aout, AudioStreamID i_stream_id)
{
    bool b_return = false;

    /* Retrieve all the stream formats supported by each output stream */
    size_t i_formats;
    AudioStreamRangedDescription *p_format_list;
    int ret = AO_GETPROP(i_stream_id, AudioStreamRangedDescription, &i_formats,
                          &p_format_list,
                          kAudioStreamPropertyAvailablePhysicalFormats,
                          kAudioObjectPropertyScopeGlobal);
    if (ret != VLC_SUCCESS)
        return false;

    for (size_t i = 0; i < i_formats; i++)
    {
#ifndef NDEBUG
        msg_Dbg(p_aout, STREAM_FORMAT_MSG("supported format: ",
                p_format_list[i].mFormat));
#endif

        if (p_format_list[i].mFormat.mFormatID == 'IAC3' ||
            p_format_list[i].mFormat.mFormatID == 'iac3' ||
            p_format_list[i].mFormat.mFormatID == kAudioFormat60958AC3 ||
            p_format_list[i].mFormat.mFormatID == kAudioFormatAC3)
            b_return = true;
    }

    free(p_format_list);
    return b_return;
}

/*
 * AudioDeviceSupportsDigital: Checks if device supports raw bitstreams
 */
static bool
AudioDeviceSupportsDigital(audio_output_t *p_aout, AudioDeviceID i_dev_id)
{
    size_t i_streams;
    AudioStreamID * p_streams;
    int ret = AO_GETPROP(i_dev_id, AudioStreamID, &i_streams, &p_streams,
                          kAudioDevicePropertyStreams,
                          kAudioObjectPropertyScopeOutput);
    if (ret != VLC_SUCCESS)
        return false;

    for (size_t i = 0; i < i_streams; i++)
    {
        if (AudioStreamSupportsDigital(p_aout, p_streams[i]))
        {
            free(p_streams);
            return true;
        }
    }

    free(p_streams);
    return false;
}

static void
ReportDevice(audio_output_t *p_aout, UInt32 i_id, char *name)
{
    char deviceid[10];
    sprintf(deviceid, "%i", i_id);

    aout_HotplugReport(p_aout, deviceid, name);
}

/*
 * AudioDeviceHasOutput: Checks if the device is actually an output device
 */
static int
AudioDeviceHasOutput(audio_output_t *p_aout, AudioDeviceID i_dev_id)
{
    size_t i_streams;
    int ret = AO_GETPROP(i_dev_id, AudioStreamID, &i_streams, NULL,
                          kAudioDevicePropertyStreams,
                          kAudioObjectPropertyScopeOutput);
    if (ret != VLC_SUCCESS || i_streams == 0)
        return FALSE;

    return TRUE;
}

static void
RebuildDeviceList(audio_output_t * p_aout)
{
    struct aout_sys_t   *p_sys = p_aout->sys;

    msg_Dbg(p_aout, "Rebuild device list");

    ReportDevice(p_aout, 0, _("System Sound Output Device"));

    /* Get number of devices */
    size_t i_devices;
    AudioDeviceID *p_devices;
    int ret = AO_GETPROP(kAudioObjectSystemObject, AudioDeviceID, &i_devices,
                         &p_devices, kAudioHardwarePropertyDevices,
                         kAudioObjectPropertyScopeGlobal);

    if (ret != VLC_SUCCESS || i_devices == 0)
    {
        msg_Err(p_aout, "No audio output devices found.");
        return;
    }
    msg_Dbg(p_aout, "found %zu audio device(s)", i_devices);

    /* setup local array */
    CFMutableArrayRef currentListOfDevices =
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

    for (size_t i = 0; i < i_devices; i++)
    {
        CFStringRef device_name_ref;
        char *psz_name;
        CFIndex length;
        UInt32 i_id = p_devices[i];

        int ret = AO_GET1PROP(i_id, CFStringRef, &device_name_ref,
                              kAudioObjectPropertyName,
                              kAudioObjectPropertyScopeGlobal);
        if (ret != VLC_SUCCESS)
        {
            msg_Dbg(p_aout, "failed to get name for device %i", i_id);
            continue;
        }

        length = CFStringGetLength(device_name_ref);
        length++;
        psz_name = malloc(length);
        if (!psz_name)
        {
            CFRelease(device_name_ref);
            return;
        }
        CFStringGetCString(device_name_ref, psz_name, length,
                           kCFStringEncodingUTF8);
        CFRelease(device_name_ref);

        msg_Dbg(p_aout, "DevID: %i DevName: %s", i_id, psz_name);

        if (!AudioDeviceHasOutput(p_aout, i_id))
        {
            msg_Dbg(p_aout, "this '%s' is INPUT only. skipping...", psz_name);
            free(psz_name);
            continue;
        }

        ReportDevice(p_aout, i_id, psz_name);
        CFNumberRef deviceNumber = CFNumberCreate(kCFAllocatorDefault,
                                                  kCFNumberSInt32Type, &i_id);
        CFArrayAppendValue(currentListOfDevices, deviceNumber);
        CFRelease(deviceNumber);

        if (AudioDeviceSupportsDigital(p_aout, i_id))
        {
            msg_Dbg(p_aout, "'%s' supports digital output", psz_name);
            char *psz_encoded_name = nil;
            asprintf(&psz_encoded_name, _("%s (Encoded Output)"), psz_name);
            i_id = i_id | AOUT_VAR_SPDIF_FLAG;
            ReportDevice(p_aout, i_id, psz_encoded_name);
            deviceNumber = CFNumberCreate(kCFAllocatorDefault,
                                          kCFNumberSInt32Type, &i_id);
            CFArrayAppendValue(currentListOfDevices, deviceNumber);
            CFRelease(deviceNumber);
            free(psz_encoded_name);
        }

        // TODO: only register once for each device
        ManageAudioStreamsCallback(p_aout, p_devices[i], true);

        free(psz_name);
    }

    vlc_mutex_lock(&p_sys->device_list_lock);
    CFIndex count = 0;
    if (p_sys->device_list)
        count = CFArrayGetCount(p_sys->device_list);
    CFRange newListSearchRange =
        CFRangeMake(0, CFArrayGetCount(currentListOfDevices));

    if (count > 0)
    {
        msg_Dbg(p_aout, "Looking for removed devices");
        CFNumberRef cfn_device_id;
        int i_device_id = 0;
        for (CFIndex x = 0; x < count; x++)
        {
            if (!CFArrayContainsValue(currentListOfDevices, newListSearchRange,
                                      CFArrayGetValueAtIndex(p_sys->device_list,
                                      x)))
            {
                cfn_device_id = CFArrayGetValueAtIndex(p_sys->device_list, x);
                if (cfn_device_id)
                {
                    CFNumberGetValue(cfn_device_id, kCFNumberSInt32Type,
                                     &i_device_id);
                    msg_Dbg(p_aout, "Device ID %i is not found in new array, "
                            "deleting.", i_device_id);

                    ReportDevice(p_aout, i_device_id, NULL);
                }
            }
        }
    }
    if (p_sys->device_list)
        CFRelease(p_sys->device_list);
    p_sys->device_list = CFArrayCreateCopy(kCFAllocatorDefault,
                                           currentListOfDevices);
    CFRelease(currentListOfDevices);
    vlc_mutex_unlock(&p_sys->device_list_lock);

    free(p_devices);
}

/*
 * Callback when current device is not alive anymore
 */
static OSStatus
DeviceAliveListener(AudioObjectID inObjectID,  UInt32 inNumberAddresses,
                    const AudioObjectPropertyAddress inAddresses[],
                    void *inClientData)
{
    VLC_UNUSED(inObjectID);
    VLC_UNUSED(inNumberAddresses);
    VLC_UNUSED(inAddresses);

    audio_output_t *p_aout = (audio_output_t *)inClientData;
    if (!p_aout)
        return -1;

    msg_Warn(p_aout, "audio device died, resetting aout");
    aout_RestartRequest(p_aout, AOUT_RESTART_OUTPUT);

    return noErr;
}

/*
 * Callback when default audio device changed
 */
static OSStatus
DefaultDeviceChangedListener(AudioObjectID inObjectID, UInt32 inNumberAddresses,
                             const AudioObjectPropertyAddress inAddresses[],
                             void *inClientData)
{
    VLC_UNUSED(inObjectID);
    VLC_UNUSED(inNumberAddresses);
    VLC_UNUSED(inAddresses);

    audio_output_t *p_aout = (audio_output_t *)inClientData;
    if (!p_aout)
        return -1;

    aout_sys_t *p_sys = p_aout->sys;

    if (!p_aout->sys->b_selected_dev_is_default)
        return noErr;

    AudioObjectID defaultDeviceID;
    int ret = AO_GET1PROP(kAudioObjectSystemObject, AudioObjectID,
                          &defaultDeviceID,
                          kAudioHardwarePropertyDefaultOutputDevice,
                          kAudioObjectPropertyScopeOutput);
    if (ret != VLC_SUCCESS)
        return -1;

    msg_Dbg(p_aout, "default device changed to %i", defaultDeviceID);

    /* Default device is changed by the os to allow other apps to play sound
     * while in digital mode. But this should not affect ourself. */
    if (p_aout->sys->b_digital)
    {
        msg_Dbg(p_aout, "ignore, as digital mode is active");
        return noErr;
    }

    vlc_mutex_lock(&p_sys->selected_device_lock);
    /* Also ignore events which announce the same device id */
    if (defaultDeviceID != p_aout->sys->i_selected_dev)
    {
        msg_Dbg(p_aout, "default device actually changed, resetting aout");
        aout_RestartRequest(p_aout, AOUT_RESTART_OUTPUT);
    }
    vlc_mutex_unlock(&p_sys->selected_device_lock);

    return noErr;
}

/*
 * Callback when physical formats for device change
 */
static OSStatus
StreamsChangedListener(AudioObjectID inObjectID, UInt32 inNumberAddresses,
                       const AudioObjectPropertyAddress inAddresses[],
                       void *inClientData)
{
    VLC_UNUSED(inNumberAddresses);
    VLC_UNUSED(inAddresses);

    audio_output_t *p_aout = (audio_output_t *)inClientData;
    if (!p_aout)
        return -1;

    aout_sys_t *p_sys = p_aout->sys;
    if(unlikely(p_sys->b_ignore_streams_changed_callback == true))
        return 0;

    msg_Dbg(p_aout, "available physical formats for audio device changed");
    RebuildDeviceList(p_aout);

    vlc_mutex_lock(&p_sys->selected_device_lock);
    /* In this case audio has not yet started. Below code will not work and is
     * not needed here. */
    if (p_sys->i_selected_dev == 0)
    {
        vlc_mutex_unlock(&p_sys->selected_device_lock);
        return 0;
    }

    /*
     * check if changed stream id belongs to current device
     */
    size_t i_streams;
    AudioStreamID *p_streams;
    int ret = AO_GETPROP(p_sys->i_selected_dev, AudioStreamID, &i_streams,
                         &p_streams, kAudioDevicePropertyStreams,
                         kAudioObjectPropertyScopeOutput);
    if (ret != VLC_SUCCESS)
    {
        vlc_mutex_unlock(&p_sys->selected_device_lock);
        return ret;
    }
    vlc_mutex_unlock(&p_sys->selected_device_lock);

    for (size_t i = 0; i < i_streams; i++)
    {
        if (p_streams[i] == inObjectID)
        {
            msg_Dbg(p_aout, "Restart aout as this affects current device");
            aout_RestartRequest(p_aout, AOUT_RESTART_OUTPUT);
            break;
        }
    }
    free(p_streams);

    return noErr;
}

/*
 * Callback when device list changed
 */
static OSStatus
DevicesListener(AudioObjectID inObjectID, UInt32 inNumberAddresses,
                const AudioObjectPropertyAddress inAddresses[],
                void *inClientData)
{
    VLC_UNUSED(inObjectID);
    VLC_UNUSED(inNumberAddresses);
    VLC_UNUSED(inAddresses);

    audio_output_t *p_aout = (audio_output_t *)inClientData;
    if (!p_aout)
        return -1;
    aout_sys_t *p_sys = p_aout->sys;

    msg_Dbg(p_aout, "audio device configuration changed, resetting cache");
    RebuildDeviceList(p_aout);

    vlc_mutex_lock(&p_sys->selected_device_lock);
    vlc_mutex_lock(&p_sys->device_list_lock);
    CFNumberRef selectedDevice =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                       &p_sys->i_selected_dev);
    CFRange range = CFRangeMake(0, CFArrayGetCount(p_sys->device_list));
    if (!CFArrayContainsValue(p_sys->device_list, range, selectedDevice))
        aout_RestartRequest(p_aout, AOUT_RESTART_OUTPUT);
    CFRelease(selectedDevice);
    vlc_mutex_unlock(&p_sys->device_list_lock);
    vlc_mutex_unlock(&p_sys->selected_device_lock);

    return noErr;
}

/*
 * StreamListener: check whether the device's physical format change is complete
 */
static OSStatus
StreamListener(AudioObjectID inObjectID, UInt32 inNumberAddresses,
               const AudioObjectPropertyAddress inAddresses[],
               void *inClientData)
{
    OSStatus err = noErr;
    struct { vlc_mutex_t lock; vlc_cond_t cond; } * w = inClientData;

    VLC_UNUSED(inObjectID);

    for (unsigned int i = 0; i < inNumberAddresses; i++)
    {
        if (inAddresses[i].mSelector == kAudioStreamPropertyPhysicalFormat)
        {
            int canc = vlc_savecancel();
            vlc_mutex_lock(&w->lock);
            vlc_cond_signal(&w->cond);
            vlc_mutex_unlock(&w->lock);
            vlc_restorecancel(canc);
            break;
        }
    }
    return err;
}

/*
 * AudioStreamChangeFormat: switch stream format based on the provided
 * description
 */
static int
AudioStreamChangeFormat(audio_output_t *p_aout, AudioStreamID i_stream_id,
                        AudioStreamBasicDescription change_format)
{
    int retValue = false;

    struct { vlc_mutex_t lock; vlc_cond_t cond; } w;

    msg_Dbg(p_aout, STREAM_FORMAT_MSG("setting stream format: ", change_format));

    /* Condition because SetProperty is asynchronious */
    vlc_cond_init(&w.cond);
    vlc_mutex_init(&w.lock);
    vlc_mutex_lock(&w.lock);

    /* Install the callback */
    int ret = AO_UPDATELISTENER(i_stream_id, true, StreamListener, &w,
                                kAudioStreamPropertyPhysicalFormat,
                                kAudioObjectPropertyScopeGlobal);

    if (ret != VLC_SUCCESS)
    {
        retValue = false;
        goto out;
    }

    /* change the format */
    ret = AO_SETPROP(i_stream_id, sizeof(AudioStreamBasicDescription),
                     &change_format, kAudioStreamPropertyPhysicalFormat,
                     kAudioObjectPropertyScopeGlobal);
    if (ret != VLC_SUCCESS)
    {
        retValue = false;
        goto out;
    }

    /* The AudioStreamSetProperty is not only asynchronious (requiring the
     * locks) it is also not atomic in its behaviour.  Therefore we check 9
     * times before we really give up.
     */
    for (int i = 0; i < 9; i++)
    {
        /* Callback is not always invoked. So first check if format is already
         * set. */
        if (i > 0)
        {
            mtime_t timeout = mdate() + 500000;
            if (vlc_cond_timedwait(&w.cond, &w.lock, timeout))
                msg_Dbg(p_aout, "reached timeout");
        }

        AudioStreamBasicDescription actual_format;
        int ret = AO_GET1PROP(i_stream_id, AudioStreamBasicDescription,
                              &actual_format,
                              kAudioStreamPropertyPhysicalFormat,
                              kAudioObjectPropertyScopeGlobal);

        if (ret != VLC_SUCCESS)
            continue;

        msg_Dbg(p_aout, STREAM_FORMAT_MSG("actual format in use: ",
                actual_format));
        if (actual_format.mSampleRate == change_format.mSampleRate &&
            actual_format.mFormatID == change_format.mFormatID &&
            actual_format.mFramesPerPacket == change_format.mFramesPerPacket)
        {
            /* The right format is now active */
            retValue = true;
            break;
        }

        /* We need to check again */
    }

out:
    vlc_mutex_unlock(&w.lock);

    /* Removing the property listener */
    ret = AO_UPDATELISTENER(i_stream_id, false, StreamListener, &w,
                            kAudioStreamPropertyPhysicalFormat,
                            kAudioObjectPropertyScopeGlobal);
    if (ret != VLC_SUCCESS)
        retValue = false;

    vlc_mutex_destroy(&w.lock);
    vlc_cond_destroy(&w.cond);

    return retValue;
}

#pragma mark -
#pragma mark core interaction

static int
SwitchAudioDevice(audio_output_t *p_aout, const char *name)
{
    struct aout_sys_t *p_sys = p_aout->sys;

    if (name)
        p_sys->i_new_selected_dev = atoi(name);
    else
        p_sys->i_new_selected_dev = 0;

    bool b_supports_digital = (p_sys->i_new_selected_dev & AOUT_VAR_SPDIF_FLAG);
    if (b_supports_digital)
        p_sys->b_selected_dev_is_digital = true;
    else
        p_sys->b_selected_dev_is_digital = false;

    p_sys->i_new_selected_dev = p_sys->i_new_selected_dev & ~AOUT_VAR_SPDIF_FLAG;

    aout_DeviceReport(p_aout, name);
    aout_RestartRequest(p_aout, AOUT_RESTART_OUTPUT);

    return 0;
}

static int
VolumeSet(audio_output_t * p_aout, float volume)
{
    struct aout_sys_t *p_sys = p_aout->sys;
    OSStatus ostatus = 0;

    if (p_sys->b_digital)
        return VLC_EGENERIC;

    p_sys->f_volume = volume;
    aout_VolumeReport(p_aout, volume);

    /* Set volume for output unit */
    if (!p_sys->b_mute)
    {
        ostatus = AudioUnitSetParameter(p_sys->au_unit,
                                        kHALOutputParam_Volume,
                                        kAudioUnitScope_Global,
                                        0,
                                        volume * volume * volume,
                                        0);
    }

    if (var_InheritBool(p_aout, "volume-save"))
        config_PutInt(p_aout, "auhal-volume",
                      lroundf(volume * AOUT_VOLUME_DEFAULT));

    return ostatus;
}

static int
MuteSet(audio_output_t * p_aout, bool mute)
{
    struct aout_sys_t *p_sys = p_aout->sys;

    if(p_sys->b_digital)
        return VLC_EGENERIC;

    p_sys->b_mute = mute;
    aout_MuteReport(p_aout, mute);

    float volume = .0;
    if (!mute)
        volume = p_sys->f_volume;

    OSStatus err =
        AudioUnitSetParameter(p_sys->au_unit, kHALOutputParam_Volume,
                              kAudioUnitScope_Global, 0,
                              volume * volume * volume, 0);

    return err == noErr ? VLC_SUCCESS : VLC_EGENERIC;
}

#pragma mark -
#pragma mark actual playback

static inline uint64_t
BytesToFrames(aout_sys_t *p_sys, size_t i_bytes)
{
    return i_bytes * p_sys->i_frame_length / p_sys->i_bytes_per_frame;
}

static inline mtime_t
FramesToUs(aout_sys_t *p_sys, uint64_t i_nb_frames)
{
    return i_nb_frames * CLOCK_FREQ / p_sys->i_rate;
}

/* Called from AudioUnit render callbacks. No lock, wait, and IO here */
static void
CopyOutput(audio_output_t *p_aout, uint8_t *p_output, size_t i_requested)
{
    aout_sys_t *p_sys = p_aout->sys;

    /* Pull audio from buffer */
    if (!atomic_load(&p_sys->b_paused))
    {
        int32_t i_available;
        void *p_data = TPCircularBufferTail(&p_sys->circular_buffer,
                                            &i_available);
        if (i_available < 0)
            i_available = 0;

        size_t i_tocopy = __MIN(i_requested, (size_t) i_available);

        if (i_tocopy > 0)
        {
            memcpy(p_output, p_data, i_tocopy);
            TPCircularBufferConsume(&p_sys->circular_buffer, i_tocopy);
        }

        /* Pad with 0 */
        if (i_requested > i_tocopy)
        {
            atomic_fetch_add(&p_sys->i_underrun_size, i_requested - i_tocopy);
            memset(&p_output[i_tocopy], 0, i_requested - i_tocopy);
        }
    }
    else
         memset(p_output, 0, i_requested);
}

/*****************************************************************************
 * RenderCallbackAnalog: This function is called everytime the AudioUnit wants
 * us to provide some more audio data.
 * Don't print anything during normal playback, calling blocking function from
 * this callback is not allowed.
 *****************************************************************************/
static OSStatus
RenderCallbackAnalog(void *p_data, AudioUnitRenderActionFlags *ioActionFlags,
                     const AudioTimeStamp *inTimeStamp,
                     UInt32 inBusNumber, UInt32 inNumberFrames,
                     AudioBufferList *ioData)
{
    VLC_UNUSED(ioActionFlags);
    VLC_UNUSED(inTimeStamp);
    VLC_UNUSED(inBusNumber);
    VLC_UNUSED(inNumberFrames);

    CopyOutput(p_data, ioData->mBuffers[0].mData,
               ioData->mBuffers[0].mDataByteSize);

    return noErr;
}

/*
 * RenderCallbackSPDIF: callback for SPDIF audio output
 */
static OSStatus
RenderCallbackSPDIF(AudioDeviceID inDevice, const AudioTimeStamp * inNow,
                    const AudioBufferList * inInputData,
                    const AudioTimeStamp * inInputTime,
                    AudioBufferList * outOutputData,
                    const AudioTimeStamp * inOutputTime, void *p_data)
{
    VLC_UNUSED(inNow);
    VLC_UNUSED(inDevice);
    VLC_UNUSED(inInputData);
    VLC_UNUSED(inInputTime);
    VLC_UNUSED(inOutputTime);

    audio_output_t * p_aout = p_data;
    aout_sys_t *p_sys = p_aout->sys;
    CopyOutput(p_aout, outOutputData->mBuffers[p_sys->i_stream_index].mData,
               outOutputData->mBuffers[p_sys->i_stream_index].mDataByteSize);

    return noErr;
}

static int
TimeGet(audio_output_t *p_aout, mtime_t *delay)
{
    struct aout_sys_t * p_sys = p_aout->sys;

    int32_t i_bytes;
    TPCircularBufferTail(&p_sys->circular_buffer, &i_bytes);

    int64_t i_frames = BytesToFrames(p_sys, i_bytes) + p_sys->i_device_latency;
    *delay = FramesToUs(p_sys, i_frames);

    return 0;
}

static void
Pause(audio_output_t *p_aout, bool pause, mtime_t date)
{
    struct aout_sys_t * p_sys = p_aout->sys;
    VLC_UNUSED(date);

    atomic_store(&p_sys->b_paused, pause);
}

static void
Flush(audio_output_t *p_aout, bool wait)
{
    struct aout_sys_t *p_sys = p_aout->sys;

    if (wait)
    {
        int32_t i_bytes;

        while (TPCircularBufferTail(&p_sys->circular_buffer, &i_bytes) != NULL)
        {
            /* Calculate the duration of the circular buffer, in order to wait
             * for the render thread to play it all */
            const mtime_t i_frame_us =
                FramesToUs(p_sys, BytesToFrames(p_sys, i_bytes)) + 10000;

            /* Don't sleep less than 10ms */
            msleep(__MAX(i_frame_us, 10000));
        }
    }
    else
    {
        /* flush circular buffer if data is left */
        TPCircularBufferClear(&p_aout->sys->circular_buffer);
    }
}

static void
Play(audio_output_t * p_aout, block_t * p_block)
{
    struct aout_sys_t *p_sys = p_aout->sys;

    if (p_block->i_nb_samples > 0)
    {
        /* Do the channel reordering */
        if (p_sys->chans_to_reorder && !p_sys->b_digital)
        {
           aout_ChannelReorder(p_block->p_buffer,
                               p_block->i_buffer,
                               p_sys->chans_to_reorder,
                               p_sys->chan_table,
                               VLC_CODEC_FL32);
        }

        /* move data to buffer */
        while (!TPCircularBufferProduceBytes(&p_sys->circular_buffer,
                                             p_block->p_buffer,
                                             p_block->i_buffer))
        {
            if (unlikely(p_block->i_buffer >
                (uint32_t) p_sys->circular_buffer.length))
            {
                msg_Err(p_aout, "the block is too big for the circular buffer");
                assert(false);
                break;
            }

            /* Wait for the render buffer to play the remaining data */
            int32_t i_avalaible_bytes;
            TPCircularBufferTail(&p_sys->circular_buffer, &i_avalaible_bytes);
            assert(i_avalaible_bytes >= 0);
            if (unlikely((size_t) i_avalaible_bytes >= p_block->i_buffer))
                continue;
            int32_t i_waiting_bytes = p_block->i_buffer - i_avalaible_bytes;

            const mtime_t i_frame_us =
                FramesToUs(p_sys, BytesToFrames(p_sys, i_waiting_bytes));

            /* Don't sleep less than 10ms */
            msleep(__MAX(i_frame_us, 10000));
        }
    }

    unsigned i_underrun_size = atomic_exchange(&p_sys->i_underrun_size, 0);
    if (i_underrun_size > 0)
        msg_Warn(p_aout, "underrun of %u bytes", i_underrun_size);

    block_Release(p_block);
}

#pragma mark -
#pragma mark initialization

/*
 * StartAnalog: open and setup a HAL AudioUnit to do PCM audio output
 */
static int
StartAnalog(audio_output_t *p_aout, audio_sample_format_t *fmt)
{
    struct aout_sys_t           *p_sys = p_aout->sys;
    OSStatus                    err = noErr;
    UInt32                      i_param_size = 0;
    int                         i_original;
    AudioComponentDescription   desc;
    AudioStreamBasicDescription DeviceFormat;
    AudioChannelLayout          *layout;
    AURenderCallbackStruct      input;
    p_aout->sys->chans_to_reorder = 0;

    SInt32 currentMinorSystemVersion;
    if (Gestalt(gestaltSystemVersionMinor, &currentMinorSystemVersion) != noErr)
        msg_Err(p_aout, "failed to check OSX version");

    /* Lets go find our Component */
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    p_sys->au_component = AudioComponentFindNext(NULL, &desc);
    if (p_sys->au_component == NULL)
    {
        msg_Err(p_aout, "cannot find any HAL component, PCM output failed");
        return VLC_EGENERIC;
    }

    err = AudioComponentInstanceNew(p_sys->au_component, &p_sys->au_unit);
    if (err != noErr)
    {
        msg_Err(p_aout, "cannot open HAL component, PCM output failed [%4.4s]",
                (const char *)&err);
        return VLC_EGENERIC;
    }

    /* Set the device we will use for this output unit */
    err = AudioUnitSetProperty(p_sys->au_unit,
                               kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0,
                               &p_sys->i_selected_dev, sizeof(AudioObjectID));

    if (err != noErr)
    {
        msg_Err(p_aout, "cannot select audio output device, PCM output failed "
                "[%4.4s]", (const char *)&err);
        goto error;
    }

    /* Get the current format */
    i_param_size = sizeof(AudioStreamBasicDescription);

    err = AudioUnitGetProperty(p_sys->au_unit,  kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 0, &DeviceFormat,
                               &i_param_size);

    if (err != noErr)
    {
        msg_Err(p_aout, "failed to detect supported stream formats [%4.4s]",
                (const char *)&err);
        goto error;
    }
    else
        msg_Dbg(p_aout, STREAM_FORMAT_MSG("current format is: ", DeviceFormat));

    /* Get the channel layout of the device side of the unit (vlc -> unit ->
     * device) */
    err = AudioUnitGetPropertyInfo(p_sys->au_unit,
                                   kAudioDevicePropertyPreferredChannelLayout,
                                   kAudioUnitScope_Output, 0, &i_param_size,
                                   NULL);

    if (err == noErr)
    {
        layout = (AudioChannelLayout *)malloc(i_param_size);

        OSStatus err =
            AudioUnitGetProperty(p_sys->au_unit,
                                 kAudioDevicePropertyPreferredChannelLayout,
                                 kAudioUnitScope_Output, 0, layout,
                                 &i_param_size);

        if (err != noErr)
            goto error;

        /* We need to "fill out" the ChannelLayout, because there are multiple
         * ways that it can be set */
        if (layout->mChannelLayoutTag ==
            kAudioChannelLayoutTag_UseChannelBitmap)
        {
            /* bitmap defined channellayout */
            err =
             AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForBitmap,
                                    sizeof(UInt32), &layout->mChannelBitmap,
                                    &i_param_size, layout);
        }
        else if (layout->mChannelLayoutTag !=
                 kAudioChannelLayoutTag_UseChannelDescriptions)
        {
            /* layouttags defined channellayout */
            err =
                AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag,
                                       sizeof(AudioChannelLayoutTag),
                                       &layout->mChannelLayoutTag,
                                       &i_param_size, layout);
        }

        if (err != noErr || layout->mNumberChannelDescriptions == 0)
        {
            msg_Err(p_aout, "insufficient number of output channels");
            free(layout);
            goto error;
        }

        msg_Dbg(p_aout, "layout of AUHAL has %i channels" ,
                layout->mNumberChannelDescriptions);

        /* Initialize the VLC core channel count */
        fmt->i_physical_channels = 0;
        i_original = fmt->i_original_channels & AOUT_CHAN_PHYSMASK;

        if (i_original == AOUT_CHAN_CENTER
         || layout->mNumberChannelDescriptions < 2)
        {
            /* We only need Mono or cannot output more than 1 channel */
            fmt->i_physical_channels = AOUT_CHAN_CENTER;
        }
        else if (i_original == (AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT)
              || layout->mNumberChannelDescriptions < 3)
        {
            /* We only need Stereo or cannot output more than 2 channels */
            fmt->i_physical_channels = AOUT_CHANS_STEREO;
        }
        else
        {
            // maps auhal channels to vlc ones
            static const unsigned i_auhal_channel_mapping[] = {
                [kAudioChannelLabel_Left]           = AOUT_CHAN_LEFT,
                [kAudioChannelLabel_Right]          = AOUT_CHAN_RIGHT,
                [kAudioChannelLabel_Center]         = AOUT_CHAN_CENTER,
                [kAudioChannelLabel_LFEScreen]      = AOUT_CHAN_LFE,
                [kAudioChannelLabel_LeftSurround]   = AOUT_CHAN_REARLEFT,
                [kAudioChannelLabel_RightSurround]  = AOUT_CHAN_REARRIGHT,
                /* needs to be swapped with rear */
                [kAudioChannelLabel_RearSurroundLeft]  = AOUT_CHAN_MIDDLELEFT,
                /* needs to be swapped with rear */
                [kAudioChannelLabel_RearSurroundRight] = AOUT_CHAN_MIDDLERIGHT,
                [kAudioChannelLabel_CenterSurround] = AOUT_CHAN_REARCENTER
            };

            /* We want more than stereo and we can do that */
            for (unsigned i = 0; i < layout->mNumberChannelDescriptions; i++)
            {
#ifndef NDEBUG
                msg_Dbg(p_aout, "this is channel: %d",
                        (int)layout->mChannelDescriptions[i].mChannelLabel);
#endif

                AudioChannelLabel chan =
                    layout->mChannelDescriptions[i].mChannelLabel;
                const size_t i_auhal_size = sizeof(i_auhal_channel_mapping)
                                          / sizeof(i_auhal_channel_mapping[0]);
                if (chan < i_auhal_size && i_auhal_channel_mapping[chan] > 0)
                    fmt->i_physical_channels |= i_auhal_channel_mapping[chan];
                else
                    msg_Dbg(p_aout, "found nonrecognized channel %d at index "
                            "%d", chan, i);
            }
            if (fmt->i_physical_channels == 0)
            {
                fmt->i_physical_channels = AOUT_CHANS_STEREO;
                msg_Err(p_aout, "You should configure your speaker layout with "
                        "Audio Midi Setup in /Applications/Utilities. VLC will "
                        "output Stereo only.");
                vlc_dialog_display_error(p_aout,
                    _("Audio device is not configured"), "%s",
                    _("You should configure your speaker layout with "
                    "\"Audio Midi Setup\" in /Applications/"
                    "Utilities. VLC will output Stereo only."));
            }
        }
        free(layout);
    }
    else
    {
        msg_Warn(p_aout, "device driver does not support "
                 "kAudioDevicePropertyPreferredChannelLayout - using stereo "
                 "fallback [%4.4s]", (const char *)&err);
        fmt->i_physical_channels = AOUT_CHANS_STEREO;
    }
    fmt->i_original_channels = fmt->i_physical_channels;

    msg_Dbg(p_aout, "selected %d physical channels for device output",
            aout_FormatNbChannels(fmt));
    msg_Dbg(p_aout, "VLC will output: %s", aout_FormatPrintChannels(fmt));

    /* Now we set the INPUT layout of the AU */
    AudioChannelLayout input_layout;
    memset (&input_layout, 0, sizeof(input_layout));
    uint32_t chans_out[AOUT_CHAN_MAX];

    /* Some channel abbreviations used below:
     * L - left
     * R - right
     * C - center
     * Ls - left surround
     * Rs - right surround
     * Cs - center surround
     * Rls - rear left surround
     * Rrs - rear right surround
     * Lw - left wide
     * Rw - right wide
     * Lsd - left surround direct
     * Rsd - right surround direct
     * Lc - left center
     * Rc - right center
     * Ts - top surround
     * Vhl - vertical height left
     * Vhc - vertical height center
     * Vhr - vertical height right
     * Lt - left matrix total. for matrix encoded stereo.
     * Rt - right matrix total. for matrix encoded stereo. */

    switch(aout_FormatNbChannels(fmt))
    {
        case 1:
            input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_Mono;
            break;
        case 2:
            input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            break;
        case 3:
            if (fmt->i_physical_channels & AOUT_CHAN_CENTER) /* L R C */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_7;
            else if (fmt->i_physical_channels & AOUT_CHAN_LFE) /* L R LFE */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_4;
            break;
        case 4:
            if (fmt->i_physical_channels & (AOUT_CHAN_CENTER | AOUT_CHAN_LFE)) /* L R C LFE */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_10;
            else if (fmt->i_physical_channels & AOUT_CHANS_REAR) /* L R Ls Rs */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_3;
            else if (fmt->i_physical_channels & AOUT_CHANS_CENTER) /* L R C Cs */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_3;
            break;
        case 5:
            if (fmt->i_physical_channels & (AOUT_CHAN_CENTER)) /* L R Ls Rs C */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_19;
            else if (fmt->i_physical_channels & (AOUT_CHAN_LFE)) /* L R Ls Rs LFE */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_18;
            break;
        case 6:
            if (fmt->i_physical_channels & (AOUT_CHAN_LFE))
            {
                /* L R Ls Rs C LFE */
                input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DVD_20;

                chans_out[0] = AOUT_CHAN_LEFT;
                chans_out[1] = AOUT_CHAN_RIGHT;
                chans_out[2] = AOUT_CHAN_REARLEFT;
                chans_out[3] = AOUT_CHAN_REARRIGHT;
                chans_out[4] = AOUT_CHAN_CENTER;
                chans_out[5] = AOUT_CHAN_LFE;

                p_aout->sys->chans_to_reorder =
                    aout_CheckChannelReorder(NULL, chans_out,
                                             fmt->i_physical_channels,
                                             p_aout->sys->chan_table);
                if (p_aout->sys->chans_to_reorder)
                    msg_Dbg(p_aout, "channel reordering needed for 5.1 output");
            }
            else
            {
                /* L R Ls Rs C Cs */
                input_layout.mChannelLayoutTag =
                    kAudioChannelLayoutTag_AudioUnit_6_0;

                chans_out[0] = AOUT_CHAN_LEFT;
                chans_out[1] = AOUT_CHAN_RIGHT;
                chans_out[2] = AOUT_CHAN_REARLEFT;
                chans_out[3] = AOUT_CHAN_REARRIGHT;
                chans_out[4] = AOUT_CHAN_CENTER;
                chans_out[5] = AOUT_CHAN_REARCENTER;

                p_aout->sys->chans_to_reorder =
                    aout_CheckChannelReorder(NULL, chans_out,
                                             fmt->i_physical_channels,
                                             p_aout->sys->chan_table);
                if (p_aout->sys->chans_to_reorder)
                    msg_Dbg(p_aout, "channel reordering needed for 6.0 output");
            }
            break;
        case 7:
            /* L R C LFE Ls Rs Cs */
            input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_6_1_A;

            chans_out[0] = AOUT_CHAN_LEFT;
            chans_out[1] = AOUT_CHAN_RIGHT;
            chans_out[2] = AOUT_CHAN_CENTER;
            chans_out[3] = AOUT_CHAN_LFE;
            chans_out[4] = AOUT_CHAN_REARLEFT;
            chans_out[5] = AOUT_CHAN_REARRIGHT;
            chans_out[6] = AOUT_CHAN_REARCENTER;

            p_aout->sys->chans_to_reorder =
                aout_CheckChannelReorder(NULL, chans_out,
                                         fmt->i_physical_channels,
                                         p_aout->sys->chan_table);
            if (p_aout->sys->chans_to_reorder)
                msg_Dbg(p_aout, "channel reordering needed for 6.1 output");

            break;
        case 8:
            if (fmt->i_physical_channels & (AOUT_CHAN_LFE)
             || currentMinorSystemVersion < 7)
            {
                /* L R C LFE Ls Rs Rls Rrs */
                input_layout.mChannelLayoutTag =
                    kAudioChannelLayoutTag_MPEG_7_1_C;

                chans_out[0] = AOUT_CHAN_LEFT;
                chans_out[1] = AOUT_CHAN_RIGHT;
                chans_out[2] = AOUT_CHAN_CENTER;
                chans_out[3] = AOUT_CHAN_LFE;
                chans_out[4] = AOUT_CHAN_MIDDLELEFT;
                chans_out[5] = AOUT_CHAN_MIDDLERIGHT;
                chans_out[6] = AOUT_CHAN_REARLEFT;
                chans_out[7] = AOUT_CHAN_REARRIGHT;

                if (!(fmt->i_physical_channels & (AOUT_CHAN_LFE)))
                    msg_Warn(p_aout, "8.0 audio output not supported on OS X "
                             "10.%i, layout will be incorrect",
                             currentMinorSystemVersion);
            }
#ifdef MAC_OS_X_VERSION_10_7
            else
            {
                /* Lc C Rc L R Ls Cs Rs */
                input_layout.mChannelLayoutTag =
                    kAudioChannelLayoutTag_DTS_8_0_B;

                chans_out[0] = AOUT_CHAN_MIDDLELEFT;
                chans_out[1] = AOUT_CHAN_CENTER;
                chans_out[2] = AOUT_CHAN_MIDDLERIGHT;
                chans_out[3] = AOUT_CHAN_LEFT;
                chans_out[4] = AOUT_CHAN_RIGHT;
                chans_out[5] = AOUT_CHAN_REARLEFT;
                chans_out[6] = AOUT_CHAN_REARCENTER;
                chans_out[7] = AOUT_CHAN_REARRIGHT;
            }
#endif
            p_aout->sys->chans_to_reorder =
                aout_CheckChannelReorder(NULL, chans_out,
                                         fmt->i_physical_channels,
                                         p_aout->sys->chan_table);
            if (p_aout->sys->chans_to_reorder)
                msg_Dbg(p_aout, "channel reordering needed for 7.1 / 8.0 output");
            break;
        case 9:
            if (currentMinorSystemVersion < 7)
            {
                msg_Warn(p_aout, "8.1 audio output not supported on OS X 10.%i",
                         currentMinorSystemVersion);
                break;
            }

#ifdef MAC_OS_X_VERSION_10_7
            /* Lc C Rc L R Ls Cs Rs LFE */
            input_layout.mChannelLayoutTag = kAudioChannelLayoutTag_DTS_8_1_B;
            chans_out[0] = AOUT_CHAN_MIDDLELEFT;
            chans_out[1] = AOUT_CHAN_CENTER;
            chans_out[2] = AOUT_CHAN_MIDDLERIGHT;
            chans_out[3] = AOUT_CHAN_LEFT;
            chans_out[4] = AOUT_CHAN_RIGHT;
            chans_out[5] = AOUT_CHAN_REARLEFT;
            chans_out[6] = AOUT_CHAN_REARCENTER;
            chans_out[7] = AOUT_CHAN_REARRIGHT;
            chans_out[8] = AOUT_CHAN_LFE;

            p_aout->sys->chans_to_reorder =
                aout_CheckChannelReorder(NULL, chans_out,
                                         fmt->i_physical_channels,
                                         p_aout->sys->chan_table);
            if (p_aout->sys->chans_to_reorder)
                msg_Dbg(p_aout, "channel reordering needed for 8.1 output");
#endif
            break;
    }

    /* Set up the format to be used */
    DeviceFormat.mSampleRate = fmt->i_rate;
    DeviceFormat.mFormatID = kAudioFormatLinearPCM;

    /* We use float 32 since this is VLC's endorsed format */
    fmt->i_format = VLC_CODEC_FL32;
    DeviceFormat.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    DeviceFormat.mBitsPerChannel = 32;
    DeviceFormat.mChannelsPerFrame = aout_FormatNbChannels(fmt);

    /* Calculate framesizes and stuff */
    DeviceFormat.mFramesPerPacket = 1;
    DeviceFormat.mBytesPerFrame = DeviceFormat.mBitsPerChannel
                                * DeviceFormat.mChannelsPerFrame / 8;
    DeviceFormat.mBytesPerPacket = DeviceFormat.mBytesPerFrame
                                 * DeviceFormat.mFramesPerPacket;

    /* Set the desired format */
    i_param_size = sizeof(AudioStreamBasicDescription);
    err = AudioUnitSetProperty(p_sys->au_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &DeviceFormat,
                               i_param_size);
    if (err != noErr)
        goto error;

    msg_Dbg(p_aout, STREAM_FORMAT_MSG("we set the AU format: " , DeviceFormat));

    /* Retrieve actual format */
    err = AudioUnitGetProperty(p_sys->au_unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &DeviceFormat,
                               &i_param_size);
    if (err != noErr)
        goto error;

    msg_Dbg(p_aout, STREAM_FORMAT_MSG("the actual set AU format is ",
            DeviceFormat));

    /* Do the last VLC aout setups */
    aout_FormatPrepare(fmt);

    /* set the IOproc callback */
    input.inputProc = RenderCallbackAnalog;
    input.inputProcRefCon = p_aout;

    err = AudioUnitSetProperty(p_sys->au_unit,
                               kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &input, sizeof(input));
    if (err != noErr)
        goto error;

    /* Set the input_layout as the layout VLC will use to feed the AU unit.
     * Yes, it must be the INPUT scope */
    err = AudioUnitSetProperty(p_sys->au_unit,
                               kAudioUnitProperty_AudioChannelLayout,
                               kAudioUnitScope_Input, 0, &input_layout,
                               sizeof(input_layout));
    if (err != noErr)
        goto error;

    /* AU initiliaze */
    err = AudioUnitInitialize(p_sys->au_unit);
    if (err != noErr)
    {
        msg_Err(p_aout, "AudioUnitInitialize failed: [%4.4s]",
                (const char *)&err);
        goto error;
    }

    /* setup circular buffer */
    TPCircularBufferInit(&p_sys->circular_buffer, AUDIO_BUFFER_SIZE_IN_SECONDS *
                         fmt->i_rate * fmt->i_bytes_per_frame);

    err = AudioOutputUnitStart(p_sys->au_unit);
    if (err != noErr)
    {
        AudioUnitUninitialize(p_sys->au_unit);
        msg_Err(p_aout, "AudioUnitInitialize failed: [%4.4s]",
                (const char *)&err);
        goto error;
    }

    /* Set volume for output unit */
    VolumeSet(p_aout, p_sys->f_volume);
    MuteSet(p_aout, p_sys->b_mute);

    return VLC_SUCCESS;
error:
    AudioComponentInstanceDispose(p_sys->au_unit);
    return VLC_EGENERIC;
}

/*
 * StartSPDIF: Setup an encoded digital stream (SPDIF) output
 */
static int
StartSPDIF(audio_output_t * p_aout, audio_sample_format_t *fmt)
{
    struct aout_sys_t       *p_sys = p_aout->sys;
    AudioStreamBasicDescription desired_stream_format;
    memset(&desired_stream_format, 0, sizeof(desired_stream_format));

    /* Start doing the SPDIF setup proces */
    p_sys->b_digital = true;

    /* Hog the device */
    p_sys->i_hog_pid = getpid();

    /*
     * HACK: On 10.6, auhal will trigger the streams changed callback when
     * calling below line, directly in the same thread. This call needs to be
     * ignored to avoid endless restarting.
     */
    p_sys->b_ignore_streams_changed_callback = true;
    int ret = AO_SETPROP(p_sys->i_selected_dev, sizeof(p_sys->i_hog_pid),
                         &p_sys->i_hog_pid, kAudioDevicePropertyHogMode,
                         kAudioObjectPropertyScopeOutput);
    p_sys->b_ignore_streams_changed_callback = false;

    if (ret != VLC_SUCCESS)
        return ret;

    if (AO_HASPROP(p_sys->i_selected_dev, kAudioDevicePropertySupportsMixing,
                   kAudioObjectPropertyScopeGlobal))
    {
        /* Set mixable to false if we are allowed to */
        UInt32 b_mix;
        bool b_writeable = AO_ISPROPSETTABLE(p_sys->i_selected_dev,
                                             kAudioDevicePropertySupportsMixing,
                                             kAudioObjectPropertyScopeGlobal);

        ret = AO_GET1PROP(p_sys->i_selected_dev, UInt32, &b_mix,
                          kAudioDevicePropertySupportsMixing,
                          kAudioObjectPropertyScopeGlobal);
        if (ret == VLC_SUCCESS && b_writeable)
        {
            b_mix = 0;
            ret = AO_SETPROP(p_sys->i_selected_dev, sizeof(UInt32), &b_mix,
                             kAudioDevicePropertySupportsMixing,
                             kAudioObjectPropertyScopeGlobal);
            p_sys->b_changed_mixing = true;
        }

        if (ret != VLC_SUCCESS)
        {
            msg_Err(p_aout, "failed to set mixmode");
            return ret;
        }
    }

    /* Get a list of all the streams on this device */
    size_t i_streams;
    AudioStreamID *p_streams;
    ret = AO_GETPROP(p_sys->i_selected_dev, AudioStreamID, &i_streams,
                     &p_streams, kAudioDevicePropertyStreams,
                     kAudioObjectPropertyScopeOutput);
    if (ret != VLC_SUCCESS)
        return ret;

    for (unsigned i = 0; i < i_streams && p_sys->i_stream_index < 0 ; i++)
    {
        /* Find a stream with a cac3 stream */
        bool b_digital = false;

        /* Retrieve all the stream formats supported by each output stream */
        size_t i_formats;
        AudioStreamRangedDescription *p_format_list;
        int ret = AO_GETPROP(p_streams[i], AudioStreamRangedDescription,
                             &i_formats, &p_format_list,
                             kAudioStreamPropertyAvailablePhysicalFormats,
                             kAudioObjectPropertyScopeGlobal);
        if (ret != VLC_SUCCESS)
            continue;

        /* Check if one of the supported formats is a digital format */
        for (size_t j = 0; j < i_formats; j++)
        {
            if (p_format_list[j].mFormat.mFormatID == 'IAC3' ||
               p_format_list[j].mFormat.mFormatID == 'iac3' ||
               p_format_list[j].mFormat.mFormatID == kAudioFormat60958AC3 ||
               p_format_list[j].mFormat.mFormatID == kAudioFormatAC3)
            {
                b_digital = true;
                break;
            }
        }

        if (b_digital)
        {
            /* if this stream supports a digital (cac3) format, then go set it.
             * */
            int i_requested_rate_format = -1;
            int i_current_rate_format = -1;
            int i_backup_rate_format = -1;

            if (!p_sys->b_revert)
            {
                /* Retrieve the original format of this stream first if not
                 * done so already */
                AudioStreamBasicDescription current_streamformat;
                int ret = AO_GET1PROP(p_streams[i], AudioStreamBasicDescription,
                                      &current_streamformat,
                                      kAudioStreamPropertyPhysicalFormat,
                                      kAudioObjectPropertyScopeGlobal);
                if (ret != VLC_SUCCESS)
                    continue;

                /* 
                 * Only the first found format id is accepted. In case of
                 * another id later on, we still use the already saved one.
                 * This can happen if the user plugs in a spdif cable while a
                 * stream is already playing. Then, auhal already misleadingly
                 * reports an ac3 format here whereas the original format
                 * should be still pcm.
                 */
                if (p_sys->sfmt_revert.mFormatID > 0
                 && p_sys->sfmt_revert.mFormatID != current_streamformat.mFormatID
                 && p_streams[i] == p_sys->i_stream_id)
                {
                    msg_Warn(p_aout, STREAM_FORMAT_MSG("Detected current stream"
                             " format: ", current_streamformat));
                    msg_Warn(p_aout, "... there is another stream format "
                             "already stored, the current one is ignored");
                }
                else
                    p_sys->sfmt_revert = current_streamformat;

                p_sys->b_revert = true;
            }

            p_sys->i_stream_id = p_streams[i];
            p_sys->i_stream_index = i;

            for (size_t j = 0; j < i_formats; j++)
            {
                if (p_format_list[j].mFormat.mFormatID == 'IAC3' ||
                   p_format_list[j].mFormat.mFormatID == 'iac3' ||
                   p_format_list[j].mFormat.mFormatID == kAudioFormat60958AC3 ||
                   p_format_list[j].mFormat.mFormatID == kAudioFormatAC3)
                {
                    if (p_format_list[j].mFormat.mSampleRate == fmt->i_rate)
                    {
                        i_requested_rate_format = j;
                        break;
                    }
                    else if (p_format_list[j].mFormat.mSampleRate ==
                             p_sys->sfmt_revert.mSampleRate)
                        i_current_rate_format = j;
                    else
                    {
                        if (i_backup_rate_format < 0
                         || p_format_list[j].mFormat.mSampleRate >
                            p_format_list[i_backup_rate_format].mFormat.mSampleRate)
                            i_backup_rate_format = j;
                    }
                }

            }

            if (i_requested_rate_format >= 0)
            {
                /* We prefer to output at the samplerate of the original audio */
                desired_stream_format =
                    p_format_list[i_requested_rate_format].mFormat;
            }
            else if (i_current_rate_format >= 0)
            {
                /* If not possible, we will try to use the current samplerate
                 * of the device */
                desired_stream_format =
                    p_format_list[i_current_rate_format].mFormat;
            }
            else
            {
                /* And if we have to, any digital format will be just fine
                 * (highest rate possible) */
                desired_stream_format =
                    p_format_list[i_backup_rate_format].mFormat;
            }
        }
        free(p_format_list);
    }
    free(p_streams);

    msg_Dbg(p_aout, STREAM_FORMAT_MSG("original stream format: ",
            p_sys->sfmt_revert));

    if (!AudioStreamChangeFormat(p_aout, p_sys->i_stream_id, desired_stream_format))
    {
        msg_Err(p_aout, "failed to change stream format for SPDIF output");
        return VLC_EGENERIC;
    }

    /* Set the format flags */
    if (desired_stream_format.mFormatFlags & kAudioFormatFlagIsBigEndian)
        fmt->i_format = VLC_CODEC_SPDIFB;
    else
        fmt->i_format = VLC_CODEC_SPDIFL;
    fmt->i_bytes_per_frame = AOUT_SPDIF_SIZE;
    fmt->i_frame_length = A52_FRAME_NB;
    fmt->i_rate = (unsigned int)desired_stream_format.mSampleRate;
    aout_FormatPrepare(fmt);

    /* Add IOProc callback */
    OSStatus err =
        AudioDeviceCreateIOProcID(p_sys->i_selected_dev,
                                  RenderCallbackSPDIF,
                                  p_aout, &p_sys->i_procID);
    if (err != noErr)
    {
        msg_Err(p_aout, "Failed to create Process ID [%4.4s]",
                (const char *)&err);
        return VLC_EGENERIC;
    }

    /* Start device */
    err = AudioDeviceStart(p_sys->i_selected_dev, p_sys->i_procID);
    if (err != noErr)
    {
        msg_Err(p_aout, "Failed to start audio device [%4.4s]",
                (const char *)&err);

        err = AudioDeviceDestroyIOProcID(p_sys->i_selected_dev, p_sys->i_procID);
        if (err != noErr)
            msg_Err(p_aout, "Failed to destroy process ID [%4.4s]",
                    (const char *)&err);

        return VLC_EGENERIC;
    }

    /* setup circular buffer */
    TPCircularBufferInit(&p_sys->circular_buffer, 200 * AOUT_SPDIF_SIZE);

    return VLC_SUCCESS;
}

static void
Stop(audio_output_t *p_aout)
{
    struct aout_sys_t   *p_sys = p_aout->sys;
    OSStatus            err = noErr;

    msg_Dbg(p_aout, "Stopping the auhal module");

    if (p_sys->au_unit)
    {
        AudioOutputUnitStop(p_sys->au_unit);
        AudioUnitUninitialize(p_sys->au_unit);
        AudioComponentInstanceDispose(p_sys->au_unit);
    }

    if (p_sys->b_digital)
    {
        /* Stop device */
        err = AudioDeviceStop(p_sys->i_selected_dev,
                               p_sys->i_procID);
        if (err != noErr)
            msg_Err(p_aout, "Failed to stop audio device [%4.4s]",
                    (const char *)&err);

        /* Remove IOProc callback */
        err = AudioDeviceDestroyIOProcID(p_sys->i_selected_dev,
                                          p_sys->i_procID);
        if (err != noErr)
            msg_Err(p_aout, "Failed to destroy Process ID [%4.4s]",
                    (const char *)&err);

        if (p_sys->b_revert
         && !AudioStreamChangeFormat(p_aout, p_sys->i_stream_id, p_sys->sfmt_revert))
            msg_Err(p_aout, "failed to revert stream format in close");

        if (p_sys->b_changed_mixing
         && p_sys->sfmt_revert.mFormatID != kAudioFormat60958AC3)
        {
            UInt32 b_mix;
            /* Revert mixable to true if we are allowed to */
            bool b_writeable =
                AO_ISPROPSETTABLE(p_sys->i_selected_dev,
                                  kAudioDevicePropertySupportsMixing,
                                  kAudioObjectPropertyScopeGlobal);
            int ret = AO_GET1PROP(p_sys->i_selected_dev, UInt32, &b_mix,
                                  kAudioDevicePropertySupportsMixing,
                                  kAudioObjectPropertyScopeOutput);
            if (ret == VLC_SUCCESS && b_writeable)
            {
                msg_Dbg(p_aout, "mixable is: %d", b_mix);
                b_mix = 1;
                ret = AO_SETPROP(p_sys->i_selected_dev,
                                 sizeof(UInt32), &b_mix,
                                 kAudioDevicePropertySupportsMixing,
                                 kAudioObjectPropertyScopeOutput);
            }

            if (ret != VLC_SUCCESS)
                msg_Err(p_aout, "failed to re-set mixmode [%4.4s]",
                        (const char *)&err);
        }
    }

    if (p_sys->i_hog_pid == getpid())
    {
        p_sys->i_hog_pid = -1;

        /*
         * HACK: On 10.6, auhal will trigger the streams changed callback when
         * calling below line, directly in the same thread. This call needs to
         * be ignored to avoid endless restarting.
         */
        p_sys->b_ignore_streams_changed_callback = true;
        AO_SETPROP(p_sys->i_selected_dev, sizeof(p_sys->i_hog_pid),
                   &p_sys->i_hog_pid, kAudioDevicePropertyHogMode,
                   kAudioObjectPropertyScopeOutput);
        p_sys->b_ignore_streams_changed_callback = false;
    }

    /* remove audio device alive callback */
    AO_UPDATELISTENER(p_sys->i_selected_dev, false, DeviceAliveListener, p_aout,
                      kAudioDevicePropertyDeviceIsAlive,
                      kAudioObjectPropertyScopeGlobal);

    p_sys->b_digital = false;

    /* clean-up circular buffer */
    TPCircularBufferCleanup(&p_sys->circular_buffer);
}

static int
Start(audio_output_t *p_aout, audio_sample_format_t *restrict fmt)
{
    UInt32                  i_param_size = 0;
    struct aout_sys_t       *p_sys = NULL;

    /* Use int here, to match kAudioDevicePropertyDeviceIsAlive
     * property size */
    int                     b_alive = false;
    bool                    b_start_digital = false;

    if (aout_FormatNbChannels(fmt) == 0)
        return VLC_EGENERIC;

    p_sys = p_aout->sys;
    p_sys->b_digital = false;
    p_sys->au_component = NULL;
    p_sys->au_unit = NULL;
    p_sys->i_hog_pid = -1;
    p_sys->i_stream_index = -1;
    p_sys->b_revert = false;
    p_sys->b_changed_mixing = false;
    p_sys->b_paused = false;
    p_sys->i_device_latency = 0;

    vlc_mutex_lock(&p_sys->selected_device_lock);
    p_sys->i_selected_dev = p_sys->i_new_selected_dev;

    aout_FormatPrint(p_aout, "VLC is looking for:", fmt);

    msg_Dbg(p_aout, "attempting to use device %i", p_sys->i_selected_dev);

    if (p_sys->i_selected_dev > 0)
    {
        /* Check if device is in devices list. Only checking for
         * kAudioDevicePropertyDeviceIsAlive is not sufficient, as a former
         * airplay device might be already gone, but the device number might be
         * still valid. Core Audio even says that this device would be alive.
         * Don't ask why, its Core Audio. */
        CFIndex count = CFArrayGetCount(p_sys->device_list);
        CFNumberRef deviceNumber =
            CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                           &p_sys->i_selected_dev);
        if (CFArrayContainsValue(p_sys->device_list,
                                 CFRangeMake(0, count), deviceNumber))
        {
            /* Check if the desired device is alive and usable */
            i_param_size = sizeof(b_alive);
            int ret = AO_GET1PROP(p_sys->i_selected_dev, int, &b_alive,
                                  kAudioDevicePropertyDeviceIsAlive,
                                  kAudioObjectPropertyScopeGlobal);
            if (ret != VLC_SUCCESS)
                b_alive = false; /* Be tolerant */

            if (!b_alive)
                msg_Warn(p_aout, "selected audio device is not alive, switching"
                         " to default device");

        }
        else
        {
            msg_Warn(p_aout, "device id %i not found in the current devices "
                     "list, fallback to default device", p_sys->i_selected_dev);
        }
        CFRelease(deviceNumber);
    }

    p_sys->b_selected_dev_is_default = false;
    if (!b_alive || p_sys->i_selected_dev == 0)
    {
        p_sys->b_selected_dev_is_default = true;

        AudioObjectID defaultDeviceID = 0;
        int ret = AO_GET1PROP(kAudioObjectSystemObject, AudioObjectID,
                              &defaultDeviceID,
                              kAudioHardwarePropertyDefaultOutputDevice,
                              kAudioObjectPropertyScopeOutput);
        if (ret != VLC_SUCCESS)
        {
            vlc_mutex_unlock(&p_sys->selected_device_lock);
            goto error;
        }
        else
            msg_Dbg(p_aout, "using default audio device %i", defaultDeviceID);

        p_sys->i_selected_dev = defaultDeviceID;
        p_sys->b_selected_dev_is_digital = true;
    }
    vlc_mutex_unlock(&p_sys->selected_device_lock);

    /* recheck if device still supports digital */
    b_start_digital = p_sys->b_selected_dev_is_digital;
    if(!AudioDeviceSupportsDigital(p_aout, p_sys->i_selected_dev))
        b_start_digital = false;

    if (b_start_digital)
        msg_Dbg(p_aout, "Using audio device for digital output");
    else
        msg_Dbg(p_aout, "Audio device supports PCM mode only");

    /* add a callback to see if the device dies later on */
    AO_UPDATELISTENER(p_sys->i_selected_dev, true, DeviceAliveListener, p_aout,
                      kAudioDevicePropertyDeviceIsAlive,
                      kAudioObjectPropertyScopeGlobal);

    int ret = AO_GET1PROP(p_sys->i_selected_dev, pid_t, &p_sys->i_hog_pid,
                          kAudioDevicePropertyHogMode,
                          kAudioObjectPropertyScopeOutput);
    if (ret != VLC_SUCCESS)
    {
        /* This is not a fatal error. Some drivers simply don't support this
         * property */
        p_sys->i_hog_pid = -1;
    }

    if (p_sys->i_hog_pid != -1 && p_sys->i_hog_pid != getpid())
    {
        msg_Err(p_aout, "Selected audio device is exclusively in use by another"
                " program.");
        vlc_dialog_display_error(p_aout, _("Audio output failed"), "%s",
            _("The selected audio output device is exclusively in "
            "use by another program."));
        goto error;
    }

    /* get device latency */
    AO_GET1PROP(p_sys->i_selected_dev, UInt32, &p_sys->i_device_latency,
                kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);
    float f_latency_in_sec = (float)p_sys->i_device_latency / (float)fmt->i_rate;
    msg_Dbg(p_aout, "Current device has a latency of %u frames (%f sec)",
            p_sys->i_device_latency, f_latency_in_sec);

    /* Ignore long Airplay latency as this is not correctly working yet */
    if (f_latency_in_sec > 0.5f)
    {
        msg_Info(p_aout, "Ignore high latency as it causes problems currently.");
        p_sys->i_device_latency = 0;
    }

    bool b_success = false;

    /* Check for Digital mode or Analog output mode */
    if (AOUT_FMT_SPDIF (fmt) && b_start_digital)
    {
        if (StartSPDIF (p_aout, fmt) == VLC_SUCCESS)
        {
            msg_Dbg(p_aout, "digital output successfully opened");
            b_success = true;
        }
    }
    else
    {
        if (StartAnalog(p_aout, fmt) == VLC_SUCCESS)
        {
            msg_Dbg(p_aout, "analog output successfully opened");
            b_success = true;
        }
    }

    if (b_success)
    {
        p_sys->i_rate = fmt->i_rate;
        p_sys->i_bytes_per_frame = fmt->i_bytes_per_frame;
        p_sys->i_frame_length = fmt->i_frame_length;

        p_aout->play = Play;
        p_aout->flush = Flush;
        p_aout->time_get = TimeGet;
        p_aout->pause = Pause;
        return VLC_SUCCESS;
    }

error:
    /* If we reach this, this aout has failed */
    msg_Err(p_aout, "opening auhal output failed");
    return VLC_EGENERIC;
}

static void Close(vlc_object_t *obj)
{
    audio_output_t *p_aout = (audio_output_t *)obj;
    aout_sys_t *p_sys = p_aout->sys;

    /* remove audio devices callback */
    AO_UPDATELISTENER(kAudioObjectSystemObject, false, DevicesListener, p_aout,
                      kAudioHardwarePropertyDevices,
                      kAudioObjectPropertyScopeGlobal);

    /* remove listener to be notified about changes in default audio device */
    AO_UPDATELISTENER(kAudioObjectSystemObject, false,
                      DefaultDeviceChangedListener, p_aout,
                      kAudioHardwarePropertyDefaultOutputDevice,
                      kAudioObjectPropertyScopeGlobal);

    /*
     * StreamsChangedListener can rebuild the device list and thus held the
     * device_list_lock.  To avoid a possible deadlock, an array copy is
     * created here.  In rare cases, this can lead to missing
     * StreamsChangedListener callback deregistration (TODO).
     */
    vlc_mutex_lock(&p_sys->device_list_lock);
    CFArrayRef device_list_cpy = CFArrayCreateCopy(NULL, p_sys->device_list);
    vlc_mutex_unlock(&p_sys->device_list_lock);

    /* remove streams callbacks */
    CFIndex count = CFArrayGetCount(device_list_cpy);
    if (count > 0)
    {
        for (CFIndex x = 0; x < count; x++)
        {
            AudioDeviceID deviceId = 0;
            CFNumberRef cfn_device_id =
                CFArrayGetValueAtIndex(device_list_cpy, x);
            if (!cfn_device_id)
                continue;

            CFNumberGetValue(cfn_device_id, kCFNumberSInt32Type, &deviceId);
            if (!(deviceId & AOUT_VAR_SPDIF_FLAG))
                ManageAudioStreamsCallback(p_aout, deviceId, false);
        }
    }

    CFRelease(device_list_cpy);
    CFRelease(p_sys->device_list);

    char *psz_device = aout_DeviceGet(p_aout);
    config_PutPsz(p_aout, "auhal-audio-device", psz_device);
    free(psz_device);

    vlc_mutex_destroy(&p_sys->selected_device_lock);
    vlc_mutex_destroy(&p_sys->device_list_lock);

    free(p_sys);
}

static int Open(vlc_object_t *obj)
{
    audio_output_t *p_aout = (audio_output_t *)obj;
    aout_sys_t *p_sys = malloc(sizeof (*p_sys));
    if (unlikely(p_sys == NULL))
        return VLC_ENOMEM;

    vlc_mutex_init(&p_sys->device_list_lock);
    vlc_mutex_init(&p_sys->selected_device_lock);
    p_sys->b_digital = false;
    p_sys->b_ignore_streams_changed_callback = false;
    p_sys->b_selected_dev_is_default = false;
    memset(&p_sys->sfmt_revert, 0, sizeof(p_sys->sfmt_revert));
    p_sys->i_stream_id = 0;
    atomic_init(&p_sys->i_underrun_size, 0);
    atomic_init(&p_sys->b_paused, false);

    p_aout->sys = p_sys;
    p_aout->start = Start;
    p_aout->stop = Stop;
    p_aout->volume_set = VolumeSet;
    p_aout->mute_set = MuteSet;
    p_aout->device_select = SwitchAudioDevice;
    p_sys->device_list = CFArrayCreate(kCFAllocatorDefault, NULL, 0, NULL);

    /*
     * Force an own run loop for callbacks.
     *
     * According to rtaudio, this is absolutely necessary since 10.6 to get
     * correct notifications.  It might fix issues when using the module as a
     * library where a proper loop is not setup already.
     */
    CFRunLoopRef theRunLoop = NULL;
    AO_SETPROP(kAudioObjectSystemObject, sizeof(CFRunLoopRef),
               &theRunLoop, kAudioHardwarePropertyRunLoop,
               kAudioObjectPropertyScopeGlobal);

    /* Attach a listener so that we are notified of a change in the device
     * setup */
    AO_UPDATELISTENER(kAudioObjectSystemObject, true, DevicesListener, p_aout,
                      kAudioHardwarePropertyDevices,
                      kAudioObjectPropertyScopeGlobal);

    /* Attach a listener to be notified about changes in default audio device */
    AO_UPDATELISTENER(kAudioObjectSystemObject, true,
                      DefaultDeviceChangedListener, p_aout,
                      kAudioHardwarePropertyDefaultOutputDevice,
                      kAudioObjectPropertyScopeGlobal);

    RebuildDeviceList(p_aout);

    /* remember the volume */
    p_sys->f_volume = var_InheritInteger(p_aout, "auhal-volume")
                    / (float)AOUT_VOLUME_DEFAULT;
    aout_VolumeReport(p_aout, p_sys->f_volume);
    p_sys->b_mute = var_InheritBool(p_aout, "mute");
    aout_MuteReport(p_aout, p_sys->b_mute);

    char *psz_audio_device = var_InheritString(p_aout, "auhal-audio-device");
    SwitchAudioDevice(p_aout, psz_audio_device);
    free(psz_audio_device);

    return VLC_SUCCESS;
}
