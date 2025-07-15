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
 * Main context for managing subgraph instances
 */
struct AVSubGraphContext {
    AVFilterGraph *graph;           ///< subgraph container
    AVFilterContext *src_filter;    ///< Source filter for input
    AVFilterContext *sink_filter;   ///< Sink filter for output
    struct AVSubCmdQueue *cmd_queue; ///< cache cmd sent when subgraph is not init
};

/**
 * @struct AVAudioFormats
 * Configuration for supported audio formats in subgraph
 */
struct AVAudioFormats {
    int sample_rate;
    enum AVSampleFormat format;
    AVChannelLayout ch_layout;
};

typedef struct AVSubGraphContext   AVSubGraphContext;
typedef struct AVAudioFormats      AVAudioFormats;

/**
 * @brief initialize and build the the subgraph.
 *
 * @param[in,out] ctx           Pointer to the subgraph context pointer.
 * @param[in]     graph_desc    subgraph description string (syntax matches
 *                              avfilter_graph_desc2()).
 * @param[in]     src_fmt       The formats of src_filter in subgraph.
 * @param[in]     sink_fmt      The formats of sink_filter in subgraph.
 *
 * @return 0 on success, negative error code on failure
 *
 * @note All parameters must be specified, and the format
 *       parameters must be deterministic values
 */
int avfilter_asubgraph_init(AVSubGraphContext *ctx,
                            const char *graph_desc,
                            const AVAudioFormats src_fmt,
                            const AVAudioFormats sink_fmt);

/**
 * @brief Release subgraph resources
 * @param[in] ctx  Pointer to the subgraph context pointer.
 */
void avfilter_asubgraph_uninit(AVSubGraphContext *ctx);

/**
 * @brief Process an audio frame through the subgraph
 * @param[in]  ctx     Initialized subgraph context
 * @param[in]  iframe  Input audio frame to process
 * @param[out] poframe The processed output audio frame of subgraph allocation.
 * @return 0 on success, negative error code on failure
 */
int avfilter_asubgraph_process(AVSubGraphContext *ctx, AVFrame *iframe, AVFrame **poframe);

/**
 * @brief Send a command to the subgraph
 * @param[in] ctx      Subgraph context
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
 * @brief Configure supported formats for the subgraph
 * @param[in]  ctx        Subgraph context
 * @param[in]  sug_fmt    Caller suggest formats to dest_filter.
 * @param[out] conf_src   Subgraph returns the best matching in_format of dest_filter.
 * @param[out] conf_sink  Subgraph returns the best matching out_format of dest_filter.
 *
 * @return 0 on success, negative error code on failure.
 *
 * @note the conf_src and conf_sink are formats supported by dest_filter, not subgraph.
 *       Subgraphs theoretically support any explicit input/output format.
 */
int avfilter_asubgraph_query_formats(const char *graph_desc, const AVAudioFormats *sug_fmt,
                                     AVAudioFormats *src_fmt, AVAudioFormats *sink_fmt);

/**
 * @brief init/reset a AVAudioFormats structure to default values.
 * @param[in,out] cfg  Pointer to AVAudioFormats structure.
 */
void avfilter_asubgraph_reinit_formats(AVAudioFormats *cfg);

#endif /* AVFILTER_SUBGRAPH_H */