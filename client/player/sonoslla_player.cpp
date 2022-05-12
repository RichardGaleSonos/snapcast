/***
    This file is part of snapcast
    Copyright (C) 2014-2021  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include <cassert>
#include <iostream>

#include <chrono>

#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"
#include "common/str_compat.hpp"
#include "common/utils/logging.hpp"
#include "common/utils/string_utils.hpp"

#include "sonoslla_player.hpp"
#include "sonoslla/llaudio.h"
#include "sonoslla/alsaaudio.h"
#include "sonoslla/fileio.h"

#include <asm/byteorder.h>

using namespace std;

extern uint32_t g_volume;

namespace player
{

static constexpr auto LOG_TAG = "SonosLLAPlayer";

static constexpr auto kAlsaDescription = "Sonos Alsa output";
static constexpr auto kLLADescription = "Sonos LLA output";

std::vector<PcmDevice> SonosLLAPlayer::pcm_list(const std::string& parameter)
{
    (void)parameter;

    std::vector<PcmDevice> result;
#ifdef SONOS_ARCH_ATTR_USES_LLA
    result.push_back(PcmDevice{0, "lla", kLLADescription});
#else
    result.push_back(PcmDevice{0, "alsa", kAlsaDescription});
#endif
    return result;
}

SonosLLAPlayer::SonosLLAPlayer(boost::asio::io_context& io_context, const ClientSettings::Player& settings, std::shared_ptr<Stream> stream)
    : Player(io_context, settings, stream), buffer_()
{
    m_num_samples = 0;

    in_instance.token = NULL;
    out_instance.token = NULL;

    bd.samples = NULL;
    bd.num_samples = 0;
}

SonosLLAPlayer::~SonosLLAPlayer()
{
    LOG(DEBUG, LOG_TAG) << "Destructor\n";
    stop();
}

void SonosLLAPlayer::start()
{
    LOG(INFO, LOG_TAG) << "Start\n";

    try
    {
        init();
    }
    catch (const SnapException& e)
    {
        LOG(ERROR, LOG_TAG) << "Exception: " << e.what() << ", code: " << e.code() << "\n";
        // Accept "Device or ressource busy", the worker loop will retry
        if (e.code() != -EBUSY)
            throw;
    }

    Player::start();
}

void SonosLLAPlayer::stop()
{
    Player::stop();

    LOG(INFO, LOG_TAG) << "Stop\n";

    uninit();
}

void SonosLLAPlayer::init()
{
	int32_t ret;
    uint32_t flags = 0;

    const SampleFormat& format = stream_->getFormat();
    uint32_t num_channels = format.channels();
    uint32_t bytes_per_sample = format.bits() / 8;

    m_num_samples = bd.num_samples = 50 * SAMPLES_PER_BUFFER;

#ifdef SONOS_ARCH_ATTR_USES_LLA
    ret = llaudio_open_output(&out_instance, AUDIODRV_DEVICE_DAC, m_num_samples, bytes_per_sample, num_channels);
    if (ret < 0) {
        throw SnapException("llaudio_open_output failed");
    }
#else
    ret = alsaaudio_open_output(&out_instance, m_num_samples, bytes_per_sample, num_channels);
    if (ret < 0) {
        throw SnapException("alsaaudio_open_output failed");
    }
#endif

    /* if output is audio, set the flag to turn amplifiers ON */
    flags |= AP_AMP_ON | AP_AUDIO_OUT;

	/* Allocate memory for the samples */
	bd.samples = (int32_t *)malloc(bd.num_samples * MAX_NUM_CHANNELS * SAMPLE_SIZE);
	if (bd.samples == NULL) {
        throw SnapException("Failed to allocate enough memory");
	}

    LOG(INFO, LOG_TAG) << "bd.samples " << bd.num_samples << " MAX_NUM_CHANNELS " << MAX_NUM_CHANNELS << " SAMPLE_SIZE " << SAMPLE_SIZE << " " << bd.num_samples * MAX_NUM_CHANNELS * SAMPLE_SIZE << " bytes\n";

	/* Setup mdp, turn ON mics/amps , set volume, channel mask, and set channel specification structure.
	 * This is not strictly required for all use cases but is required for most
	 * and it doesn't hurt setting up mdp and channel spec every time */
    ret = platform_init(flags, 1, 0xffff, &in_instance, &out_instance);
	if (ret < 0) {
        throw SnapException("platform_init failed");
    }
}

void SonosLLAPlayer::uninit()
{
	platform_exit(false);

    if (in_instance.token) {
        in_instance.close_fun(in_instance.token);
    }
    if (out_instance.token) {
        out_instance.close_fun(out_instance.token);
    }

	/* Main loop exited; We got here because user pressed ctrl-c or play length expired
	 * or something failed during initialization. Close instances and free memory */
	if (bd.samples != NULL) {
		free(bd.samples);
        bd.samples = NULL;
	}
}

bool SonosLLAPlayer::needsThread() const
{
    return true;
}

void SonosLLAPlayer::worker()
{
    while (active_)
    {
        int time = bd.num_samples / stream_->getFormat().msRate();

        // if we are still active, wait for a chunk and attempt to reconnect
        while (active_ && !stream_->waitForChunk(std::chrono::milliseconds(100)))
        {
            static utils::logging::TimeConditional cond(std::chrono::seconds(2));
            LOG(DEBUG, LOG_TAG) << cond << "Waiting for a chunk to become available before reconnecting\n";
        }

        auto numFrames = bd.num_samples;
        auto needed = numFrames * stream_->getFormat().frameSize();
        if (buffer_.size() < needed)
            buffer_.resize(needed);

        if (!stream_->getPlayerChunkOrSilence(buffer_.data(), std::chrono::milliseconds(time), numFrames))
        {
            LOG(INFO, LOG_TAG) << "Failed to get chunk. Playing silence.\n";
        }
        else
        {
            adjustVolume(static_cast<char*>(buffer_.data()), numFrames);
        }

        const SampleFormat& format = stream_->getFormat();
        uint32_t num_channels = format.channels();
        uint32_t bytes_per_sample = format.bits() / 8;

#if 0
        LOG(INFO, LOG_TAG) << "copying numFrames:" << numFrames << " num_channels:" << num_channels << " bytes_per_sample:" << bytes_per_sample << "\n";
        memcpy(bd.samples, buffer_.data(), numFrames * num_channels * bytes_per_sample);
#endif
#if 0
        LOG(INFO, LOG_TAG) << "swapping numFrames:" << numFrames << " num_channels:" << num_channels << " bytes_per_sample:" << bytes_per_sample << "\n";
        size_t i;
        int32_t* p = (int32_t*)&buffer_[0];
        for (i = 0; i < numFrames * num_channels; i++) {
            bd.samples[i] = p[i];
            // bd.samples[i] = __le32_to_cpu(p[i]);
        }
#endif
#if 1
        LOG(INFO, LOG_TAG) << "swizzling numFrames:" << numFrames << " num_channels:" << num_channels << " bytes_per_sample:" << bytes_per_sample << "\n";
        size_t i;
        int16_t* p = (int16_t*)&buffer_[0];
        for (i = 0; i < numFrames * num_channels; i++) {
        	int32_t temp_sample = 0;
            memcpy((void *)&temp_sample, &p[i], bytes_per_sample);
            bd.samples[i] = __le32_to_cpu(temp_sample);
        }
#endif
		bd.num_samples = numFrames;
        // g_volume = (int)(volume_ * 100);

        LOG(INFO, LOG_TAG) << "writing " << bd.num_samples << "\n";
        out_instance.write_fun(out_instance.token, &bd);
    }
}

}
