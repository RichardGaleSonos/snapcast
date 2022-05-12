/***
    This file is part of snapcast
    Copyright (C) 2014-2020  Johannes Pohl

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

#ifndef SONOSLLA_PLAYER_HPP
#define SONOSLLA_PLAYER_HPP

#include "player.hpp"
#include <cstdio>
#include <memory>

#include "sonoslla/common.h"

/* sat configuration structure. Argp updates this structure based on options and arguments */
typedef struct sat_config_struct {
	size_t total_samples_in_wave;
	uint32_t bytes_per_sample;
	uint32_t num_channels;
	uint32_t frequency;
	uint32_t play_length;
	uint32_t input_type;
	uint32_t output_type;
	uint32_t wave_type;
	uint32_t volume;
	uint32_t channel_mask;
	uint32_t mic_channel;
	int32_t num_loops;
	char input_file[4096];
	char output_file[4096];
}sat_config_t;

namespace player
{

static constexpr auto SONOSLLA = "sonoslla";

class SonosLLAPlayer : public Player
{
public:
    SonosLLAPlayer(boost::asio::io_context& io_context, const ClientSettings::Player& settings, std::shared_ptr<Stream> stream);
    virtual ~SonosLLAPlayer();

    void start() override;
    void stop() override;

    /// List the system's audio output devices
    static std::vector<PcmDevice> pcm_list(const std::string& parameter);

protected:
    bool needsThread() const override;
    void worker() override;

    void init();
    void uninit();

    std::vector<char> buffer_;

    size_t m_num_samples;
    sat_plugin_inst_t in_instance;
    sat_plugin_inst_t out_instance;
    sat_buff_desc_t bd;
    // sat_config_t config;
};

} // namespace player

#endif
