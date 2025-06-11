/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * AV_SubGraph Management
 */

#ifndef AVFILTER_SUBGRAPH_H
#define AVFILTER_SUBGRAPH_H

#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include "avfilter.h"

#include <sys/queue.h>

typedef struct AVSubCmd {
    SIMPLEQ_ENTRY(AVSubCmd) entry;
    char                  *cmd;
} AVSubCmd;

SIMPLEQ_HEAD(AVSubCmdQueue, AVSubCmd);

/**
 * @struct AVSubGraphContext
 * Main context for managing filter graph instances
 */
struct AVSubGraphContext {
    AVFilterGraph *graph;           ///< Filter graph container
    AVFilterContext *src_filter;    ///< Source filter for input
    AVFilterContext *sink_filter;   ///< Sink filter for output
    struct AVSubCmdQueue *cmd_queue; ///< cache cmd sent when subgraph is not init
};

/**
 * @struct AVSubGraphFormats
 * Configuration for supported audio formats in filter graph
 */
struct AVSubGraphFormats {
    int nb_sample_rates;            ///< Number of supported sample rates
    int nb_formats;                 ///< Number of supported sample formats
    int nb_channel_layouts;         ///< Number of supported channel layouts
    int period_time;

    int *sample_rates;              ///< Array of supported sample rates
    enum AVSampleFormat *formats;   ///< Array of supported sample formats
    AVChannelLayout *channel_layouts; ///< Array of supported channel layouts
};

typedef struct AVSubGraphContext   AVSubGraphContext;
typedef struct AVSubGraphFormats   AVSubGraphFormats;

/**
 * @brief Alloc and initialize the AVFilter GraphContext context,
 *        and then build the filter graph
 *
 * @param[in,out] ctx           Pointer to the filter graph context pointer.
 * @param[in]     graph_desc   Filter graph description string (syntax matches
 *                              avfilter_graph_desc2()).
 * @param[in]     in_sample_rate    Input sample rate in Hz.
 * @param[in]     out_sample_rate   Output sample rate in Hz.
 * @param[in]     in_format         Input sample format.
 * @param[in]     out_format        Output sample format.
 * @param[in]     in_ch_layout      Input channel layout.
 * @param[in]     out_ch_layout     Output channel layout.
 *
 * @return 0 on success, negative error code on failure
 *
 * @note All parameters must be specified, and the format
 *       parameters must be deterministic values
 */
int avfilter_asubgraph_init(AVSubGraphContext **ctxp,
                            const char *graph_desc,
                            int in_sample_rate,
                            int out_sample_rate,
                            enum AVSampleFormat in_format,
                            enum AVSampleFormat out_format,
                            AVChannelLayout in_ch_layout,
                            AVChannelLayout out_ch_layout);

/**
 * @brief Release filter graph resources and AVSubGraphContext
 * @param[in] ctx  Pointer to the filter graph context pointer.
 */
void avfilter_asubgraph_uninit(AVSubGraphContext **ctxp);

/**
 * @brief Process an audio frame through the filter graph
 * @param[in] ctx    Initialized filter graph context
 * @param[in] frame  Input audio frame to process
 * @return 0 on success, negative error code on failure
 */
int avfilter_asubgraph_process(AVSubGraphContext *ctx, AVFrame *frame);

/**
 * @brief Send a command to the filter graph
 * @param[in] ctx      Initialized filter graph context
 * @param[in] cmd      Command string to execute
 * @param[in] args     Command arguments
 * @param[out] res     Buffer for command response
 * @param[out] res_len Length of response buffer
 * @param[in] flags    Execution flags
 * @return 0 on success, negative error code on failure
 */
int avfilter_asubgraph_process_command(AVSubGraphContext *ctx, const char *cmd, const char *args,
                                       char *res, int res_len, int flags);

/**
 * @brief Configure supported formats for the filter graph
 * @param[in]  ctx      Filter graph context
 * @param[out] cfg_in   Receives input format configuration (auto-allocated)
 * @param[out] cfg_out  Receives output format configuration (auto-allocated)
 * @note Caller must free configurations with avfilter_asubgraph_fmtconfig_free()
 * @return 0 on success, negative error code on failure
 */
int avfilter_asubgraph_query_formats(const char *graph_desc, AVSubGraphFormats **cfg_in,
                                     AVSubGraphFormats **cfg_out);

/**
 * @brief Free a format configuration structure
 * @param[in,out] cfg  Double pointer to configuration to free
 */
void avfilter_asubgraph_free_formats(AVSubGraphFormats **cfg);

#endif /* AVFILTER_SUBGRAPH_H */