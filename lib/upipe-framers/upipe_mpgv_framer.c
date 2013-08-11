/*
 * Copyright (C) 2013 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
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
 */

/** @file
 * @short Upipe module building frames from chunks of an ISO 13818-2 stream
 */

#include <upipe/ubase.h>
#include <upipe/ulist.h>
#include <upipe/uprobe.h>
#include <upipe/uref.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_block.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_pic.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_clock.h>
#include <upipe/uclock.h>
#include <upipe/ubuf.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_sync.h>
#include <upipe/upipe_helper_uref_stream.h>
#include <upipe/upipe_helper_output.h>
#include <upipe-framers/upipe_mpgv_framer.h>
#include <upipe-framers/uref_mpgv.h>
#include <upipe-framers/uref_mpgv_flow.h>

#include "upipe_framers_common.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <bitstream/mpeg/mp2v.h>

/** @internal @This translates the MPEG frame_rate_code to urational. */
static const struct urational frame_rate_from_code[] = {
    { .num = 0, .den = 0 }, /* invalid */
    { .num = 24000, .den = 1001 },
    { .num = 24, .den = 1 },
    { .num = 25, .den = 1 },
    { .num = 30000, .den = 1001 },
    { .num = 30, .den = 1 },
    { .num = 50, .den = 1 },
    { .num = 60000, .den = 1001 },
    { .num = 60, .den = 1 },
    /* Xing */
    { .num = 15000, .den = 1001 },
    /* libmpeg3 */
    { .num = 5000, .den = 1001 },
    { .num = 10000, .den = 1001 },
    { .num = 12000, .den = 1001 },
    { .num = 15000, .den = 1001 },
    /* invalid */
    { .num = 0, .den = 0 },
    { .num = 0, .den = 0 }
};

/** @internal @This is the private context of an mpgvf pipe. */
struct upipe_mpgvf {
    /* output stuff */
    /** pipe acting as output */
    struct upipe *output;
    /** output flow definition packet */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;
    /** input flow definition packet */
    struct uref *flow_def_input;
    /** last random access point */
    uint64_t systime_rap;
    /** random access point of the last ref */
    uint64_t systime_rap_ref;

    /* picture parsing stuff */
    /** last output picture number */
    uint64_t last_picture_number;
    /** last temporal reference read from the stream, or -1 */
    int last_temporal_reference;
    /** true we have had a discontinuity recently */
    bool got_discontinuity;
    /** true if the user wants us to insert sequence headers before I frames,
     * if it is not already present */
    bool insert_sequence;
    /** pointer to a sequence header */
    struct ubuf *sequence_header;
    /** pointer to a sequence header extension */
    struct ubuf *sequence_ext;
    /** pointer to a sequence display extension */
    struct ubuf *sequence_display;
    /** true if the flag progressive sequence is true */
    bool progressive_sequence;
    /** frames per second */
    struct urational fps;
    /** closed GOP */
    bool closed_gop;
    /** sample aspect ratio */
    struct urational sar;

    /* octet stream stuff */
    /** next uref to be processed */
    struct uref *next_uref;
    /** original size of the next uref */
    size_t next_uref_size;
    /** urefs received after next uref */
    struct ulist urefs;

    /* octet stream parser stuff */
    /** context of the scan function */
    uint32_t scan_context;
    /** current size of next frame (in next_uref) */
    size_t next_frame_size;
    /** true if the next uref begins with a sequence header */
    bool next_frame_sequence;
    /** offset of the sequence extension in next_uref, or -1 */
    ssize_t next_frame_sequence_ext_offset;
    /** offset of the sequence display in next_uref, or -1 */
    ssize_t next_frame_sequence_display_offset;
    /** offset of the GOP header in next_uref, or -1 */
    ssize_t next_frame_gop_offset;
    /** offset of the picture header in next_uref, or -1 */
    ssize_t next_frame_offset;
    /** offset of the picture extension in next_uref, or -1 */
    ssize_t next_frame_ext_offset;
    /** true if we have found at least one slice header */
    bool next_frame_slice;
    /** original PTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_pts_orig;
    /** PTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_pts;
    /** system PTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_pts_sys;
    /** original DTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_dts_orig;
    /** DTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_dts;
    /** system DTS of the next picture, or UINT64_MAX */
    uint64_t next_frame_dts_sys;
    /** true if we have thrown the sync_acquired event (that means we found a
     * sequence header) */
    bool acquired;

    /** public upipe structure */
    struct upipe upipe;
};

/** @hidden */
static void upipe_mpgvf_promote_uref(struct upipe *upipe);

UPIPE_HELPER_UPIPE(upipe_mpgvf, upipe)
UPIPE_HELPER_FLOW(upipe_mpgvf, UPIPE_MPGVF_EXPECTED_FLOW_DEF)
UPIPE_HELPER_SYNC(upipe_mpgvf, acquired)
UPIPE_HELPER_UREF_STREAM(upipe_mpgvf, next_uref, next_uref_size, urefs,
                         upipe_mpgvf_promote_uref)

UPIPE_HELPER_OUTPUT(upipe_mpgvf, output, flow_def, flow_def_sent)

/** @internal @This flushes all PTS timestamps.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mpgvf_flush_pts(struct upipe *upipe)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    upipe_mpgvf->next_frame_pts_orig = UINT64_MAX;
    upipe_mpgvf->next_frame_pts = UINT64_MAX;
    upipe_mpgvf->next_frame_pts_sys = UINT64_MAX;
}

/** @internal @This flushes all DTS timestamps.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mpgvf_flush_dts(struct upipe *upipe)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    upipe_mpgvf->next_frame_dts_orig = UINT64_MAX;
    upipe_mpgvf->next_frame_dts = UINT64_MAX;
    upipe_mpgvf->next_frame_dts_sys = UINT64_MAX;
}

/** @internal @This allocates an mpgvf pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_mpgvf_alloc(struct upipe_mgr *mgr,
                                       struct uprobe *uprobe,
                                       uint32_t signature, va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_mpgvf_alloc_flow(mgr, uprobe, signature, args,
                                                 &flow_def);
    if (unlikely(upipe == NULL))
        return NULL;

    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    upipe_mpgvf_init_sync(upipe);
    upipe_mpgvf_init_uref_stream(upipe);
    upipe_mpgvf_init_output(upipe);
    upipe_mpgvf->flow_def_input = flow_def;
    upipe_mpgvf->systime_rap = UINT64_MAX;
    upipe_mpgvf->systime_rap_ref = UINT64_MAX;
    upipe_mpgvf->last_picture_number = 0;
    upipe_mpgvf->last_temporal_reference = -1;
    upipe_mpgvf->got_discontinuity = false;
    upipe_mpgvf->insert_sequence = false;
    upipe_mpgvf->scan_context = UINT32_MAX;
    upipe_mpgvf->next_frame_size = 0;
    upipe_mpgvf->next_frame_sequence = false;
    upipe_mpgvf->next_frame_sequence_ext_offset = -1;
    upipe_mpgvf->next_frame_sequence_display_offset = -1;
    upipe_mpgvf->next_frame_gop_offset = -1;
    upipe_mpgvf->next_frame_offset = -1;
    upipe_mpgvf->next_frame_ext_offset = -1;
    upipe_mpgvf->next_frame_slice = false;
    upipe_mpgvf_flush_pts(upipe);
    upipe_mpgvf_flush_dts(upipe);
    upipe_mpgvf->sequence_header = upipe_mpgvf->sequence_ext =
        upipe_mpgvf->sequence_display = NULL;
    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This finds an MPEG-2 start code and returns its value.
 *
 * @param upipe description structure of the pipe
 * @param start_p filled in with the value of the start code
 * @param next_p filled in with the value of the extension code, if applicable
 * @return true if a start code was found
 */
static bool upipe_mpgvf_find(struct upipe *upipe,
                             uint8_t *start_p, uint8_t *next_p)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    const uint8_t *buffer;
    int size = -1;
    while (uref_block_read(upipe_mpgvf->next_uref, upipe_mpgvf->next_frame_size,
                           &size, &buffer)) {
        const uint8_t *p = upipe_framers_mpeg_scan(buffer, buffer + size,
                                                   &upipe_mpgvf->scan_context);
        if (p < buffer + size)
            *next_p = *p;
        uref_block_unmap(upipe_mpgvf->next_uref, upipe_mpgvf->next_frame_size);

        if ((upipe_mpgvf->scan_context & 0xffffff00) == 0x100) {
            *start_p = upipe_mpgvf->scan_context & 0xff;
            upipe_mpgvf->next_frame_size += p - buffer;
            if (*start_p == MP2VX_START_CODE && p >= buffer + size &&
                !uref_block_extract(upipe_mpgvf->next_uref,
                                    upipe_mpgvf->next_frame_size, 1, next_p)) {
                upipe_mpgvf->scan_context = UINT32_MAX;
                upipe_mpgvf->next_frame_size -= 4;
                return false;
            }
            return true;
        }
        upipe_mpgvf->next_frame_size += size;
        size = -1;
    }
    return false;
}

/** @internal @This parses a new sequence header, and outputs a flow
 * definition
 *
 * @param upipe description structure of the pipe
 * @return false in case of error
 */
static bool upipe_mpgvf_parse_sequence(struct upipe *upipe)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    uint8_t sequence_buffer[MP2VSEQ_HEADER_SIZE];
    const uint8_t *sequence;
    if (unlikely((sequence = ubuf_block_peek(upipe_mpgvf->sequence_header,
                                             0, MP2VSEQ_HEADER_SIZE,
                                             sequence_buffer)) == NULL)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }
    uint16_t horizontal = mp2vseq_get_horizontal(sequence);
    uint16_t vertical = mp2vseq_get_vertical(sequence);
    uint8_t aspect = mp2vseq_get_aspect(sequence);
    uint8_t framerate = mp2vseq_get_framerate(sequence);
    uint32_t bitrate = mp2vseq_get_bitrate(sequence);
    uint32_t vbvbuffer = mp2vseq_get_vbvbuffer(sequence);
    if (unlikely(!ubuf_block_peek_unmap(upipe_mpgvf->sequence_header, 0,
                                        sequence_buffer, sequence))) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    struct urational frame_rate = frame_rate_from_code[framerate];
    if (!frame_rate.num) {
        upipe_err_va(upipe, "invalid frame rate %d", framerate);
        return false;
    }

    struct uref *flow_def = uref_dup(upipe_mpgvf->flow_def_input);
    if (unlikely(flow_def == NULL)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }
    bool ret = true;

    uint64_t max_octetrate = 1500000 / 8;
    bool progressive = true;
    uint8_t chroma = MP2VSEQX_CHROMA_420;
    if (upipe_mpgvf->sequence_ext != NULL) {
        uint8_t ext_buffer[MP2VSEQX_HEADER_SIZE];
        const uint8_t *ext;
        if (unlikely((ext = ubuf_block_peek(upipe_mpgvf->sequence_ext,
                                            0, MP2VSEQX_HEADER_SIZE,
                                            ext_buffer)) == NULL)) {
            uref_free(flow_def);
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }

        uint8_t profilelevel = mp2vseqx_get_profilelevel(ext);
        progressive = mp2vseqx_get_progressive(ext);
        chroma = mp2vseqx_get_chroma(ext);
        horizontal |= mp2vseqx_get_horizontal(ext) << 12;
        vertical |= mp2vseqx_get_vertical(ext) << 12;
        bitrate |= mp2vseqx_get_bitrate(ext) << 18;
        vbvbuffer |= mp2vseqx_get_vbvbuffer(ext) << 10;
        bool lowdelay = mp2vseqx_get_lowdelay(ext);
        frame_rate.num *= mp2vseqx_get_frameraten(ext) + 1;
        frame_rate.den *= mp2vseqx_get_framerated(ext) + 1;
        urational_simplify(&frame_rate);

        if (unlikely(!ubuf_block_peek_unmap(upipe_mpgvf->sequence_ext, 0,
                                            ext_buffer, ext))) {
            uref_free(flow_def);
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }

        ret = ret && uref_mpgv_flow_set_profilelevel(flow_def, profilelevel);
        switch (profilelevel & MP2VSEQX_LEVEL_MASK) {
            case MP2VSEQX_LEVEL_LOW:
                max_octetrate = 4000000 / 8;
                break;
            case MP2VSEQX_LEVEL_MAIN:
                max_octetrate = 15000000 / 8;
                break;
            case MP2VSEQX_LEVEL_HIGH1440:
                max_octetrate = 60000000 / 8;
                break;
            case MP2VSEQX_LEVEL_HIGH:
                max_octetrate = 80000000 / 8;
                break;
            default:
                upipe_err_va(upipe, "invalid level %d",
                             profilelevel & MP2VSEQX_LEVEL_MASK);
                uref_free(flow_def);
                return false;
        }
        if (lowdelay)
            ret = ret && uref_flow_set_lowdelay(flow_def);
    } else
        upipe_mpgvf->progressive_sequence = true;

    ret = ret && uref_pic_flow_set_fps(flow_def, frame_rate);
    ret = ret && uref_block_flow_set_max_octetrate(flow_def, max_octetrate);
    upipe_mpgvf->progressive_sequence = progressive;
    ret = ret && uref_pic_flow_set_macropixel(flow_def, 1);
    ret = ret && uref_pic_flow_set_planes(flow_def, 0);
    ret = ret && uref_pic_flow_add_plane(flow_def, 1, 1, 1, "y8");
    switch (chroma) {
        case MP2VSEQX_CHROMA_420:
            ret = ret && uref_pic_flow_add_plane(flow_def, 2, 2, 1, "u8");
            ret = ret && uref_pic_flow_add_plane(flow_def, 2, 2, 1, "v8");
            ret = ret && uref_flow_set_def(flow_def,
                    UPIPE_MPGVF_EXPECTED_FLOW_DEF "pic.planar8_8_420.");
            break;
        case MP2VSEQX_CHROMA_422:
            ret = ret && uref_pic_flow_add_plane(flow_def, 2, 1, 1, "u8");
            ret = ret && uref_pic_flow_add_plane(flow_def, 2, 1, 1, "v8");
            ret = ret && uref_flow_set_def(flow_def,
                    UPIPE_MPGVF_EXPECTED_FLOW_DEF "pic.planar8_8_422.");
            break;
        case MP2VSEQX_CHROMA_444:
            ret = ret && uref_pic_flow_add_plane(flow_def, 1, 1, 1, "u8");
            ret = ret && uref_pic_flow_add_plane(flow_def, 1, 1, 1, "v8");
            ret = ret && uref_flow_set_def(flow_def,
                    UPIPE_MPGVF_EXPECTED_FLOW_DEF "pic.planar8_8_444.");
            break;
        default:
            upipe_err_va(upipe, "invalid chroma format %d", chroma);
            uref_free(flow_def);
            return false;
    }

    ret = ret && uref_pic_set_hsize(flow_def, horizontal);
    ret = ret && uref_pic_set_vsize(flow_def, vertical);
    switch (aspect) {
        case MP2VSEQ_ASPECT_SQUARE:
            upipe_mpgvf->sar.num = upipe_mpgvf->sar.den = 1;
            break;
        case MP2VSEQ_ASPECT_4_3:
            upipe_mpgvf->sar.num = vertical * 4;
            upipe_mpgvf->sar.den = horizontal * 3;
            urational_simplify(&upipe_mpgvf->sar);
            break;
        case MP2VSEQ_ASPECT_16_9:
            upipe_mpgvf->sar.num = vertical * 16;
            upipe_mpgvf->sar.den = horizontal * 9;
            urational_simplify(&upipe_mpgvf->sar);
            break;
        case MP2VSEQ_ASPECT_2_21:
            upipe_mpgvf->sar.num = vertical * 221;
            upipe_mpgvf->sar.den = horizontal * 100;
            urational_simplify(&upipe_mpgvf->sar);
            break;
        default:
            upipe_err_va(upipe, "invalid aspect ratio %d", aspect);
            uref_free(flow_def);
            return false;
    }
    ret = ret && uref_pic_set_aspect(flow_def, upipe_mpgvf->sar);
    upipe_mpgvf->fps = frame_rate;
    ret = ret && uref_block_flow_set_octetrate(flow_def, bitrate * 400 / 8);
    ret = ret && uref_block_flow_set_cpb_buffer(flow_def,
                                                vbvbuffer * 16 * 1024 / 8);

    if (upipe_mpgvf->sequence_display != NULL) {
        size_t size;
        uint8_t display_buffer[MP2VSEQDX_HEADER_SIZE + MP2VSEQDX_COLOR_SIZE];
        const uint8_t *display;
        if (unlikely(!ubuf_block_size(upipe_mpgvf->sequence_display, &size) ||
                     (display = ubuf_block_peek(upipe_mpgvf->sequence_display,
                                                0, size,
                                                display_buffer)) == NULL)) {
            uref_free(flow_def);
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }

        uint16_t display_horizontal = mp2vseqdx_get_horizontal(display);
        uint16_t display_vertical = mp2vseqdx_get_vertical(display);

        if (unlikely(!ubuf_block_peek_unmap(upipe_mpgvf->sequence_display, 0,
                                            display_buffer, display))) {
            uref_free(flow_def);
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }

        ret = ret && uref_pic_set_hsize_visible(flow_def, display_horizontal);
        ret = ret && uref_pic_set_vsize_visible(flow_def, display_vertical);
    }

    if (unlikely(!ret)) {
        uref_free(flow_def);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }
    upipe_mpgvf_store_flow_def(upipe, flow_def);
    return true;
}

/** @internal @This extracts the sequence header from a uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @return pointer to ubuf containing only the sequence header
 */
static struct ubuf *upipe_mpgvf_extract_sequence(struct upipe *upipe,
                                                 struct uref *uref)
{
    uint8_t word;
    if (unlikely(!uref_block_extract(uref, 11, 1, &word)))
        return NULL;

    size_t sequence_header_size = MP2VSEQ_HEADER_SIZE;
    if (word & 0x2) {
        /* intra quantiser matrix */
        sequence_header_size += 64;
        if (unlikely(!uref_block_extract(uref, 11 + 64, 1, &word)))
            return NULL;
    }
    if (word & 0x1) {
        /* non-intra quantiser matrix */
        sequence_header_size += 64;
    }

    return ubuf_block_splice(uref->ubuf, 0, sequence_header_size);
}

/** @internal @This extracts the sequence extension from a uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @param offset offset of the sequence extension in the uref
 * @return pointer to ubuf containing only the sequence extension
 */
static struct ubuf *upipe_mpgvf_extract_extension(struct upipe *upipe,
                                                  struct uref *uref,
                                                  size_t offset)
{
    return ubuf_block_splice(uref->ubuf, offset, MP2VSEQX_HEADER_SIZE);
}

/** @internal @This extracts the sequence display extension from a uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @return pointer to ubuf containing only the sequence extension
 */
static struct ubuf *upipe_mpgvf_extract_display(struct upipe *upipe,
                                                struct uref *uref,
                                                size_t offset)
{
    uint8_t word;
    if (unlikely(!uref_block_extract(uref, offset + 4, 1, &word)))
        return NULL;
    return ubuf_block_splice(uref->ubuf, offset, MP2VSEQDX_HEADER_SIZE + 
                                   ((word & 0x1) ? MP2VSEQDX_COLOR_SIZE : 0));
}

/** @internal @This handles a uref containing a sequence header.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame, beginning with a sequence header
 * @return false in case of error
 */
static bool upipe_mpgvf_handle_sequence(struct upipe *upipe, struct uref *uref)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    struct ubuf *sequence_ext = NULL;
    struct ubuf *sequence_display = NULL;
    struct ubuf *sequence_header = upipe_mpgvf_extract_sequence(upipe, uref);
    if (unlikely(sequence_header == NULL))
        return false;

    if (upipe_mpgvf->next_frame_sequence_ext_offset != -1) {
        sequence_ext = upipe_mpgvf_extract_extension(upipe, uref,
                upipe_mpgvf->next_frame_sequence_ext_offset);
        if (unlikely(sequence_ext == NULL)) {
            ubuf_free(sequence_header);
            return false;
        }

        if (upipe_mpgvf->next_frame_sequence_display_offset != -1) {
            sequence_display = upipe_mpgvf_extract_display(upipe, uref,
                    upipe_mpgvf->next_frame_sequence_display_offset);
            if (unlikely(sequence_display == NULL)) {
                ubuf_free(sequence_header);
                ubuf_free(sequence_ext);
                return false;
            }
        }
    }

    if (likely(upipe_mpgvf->sequence_header != NULL &&
               ubuf_block_equal(sequence_header,
                                  upipe_mpgvf->sequence_header) &&
               ((upipe_mpgvf->sequence_ext == NULL && sequence_ext == NULL) ||
                (upipe_mpgvf->sequence_ext != NULL && sequence_ext != NULL &&
                 ubuf_block_equal(sequence_ext,
                                  upipe_mpgvf->sequence_ext))) &&
               ((upipe_mpgvf->sequence_display == NULL &&
                 sequence_display == NULL) ||
                (upipe_mpgvf->sequence_display != NULL &&
                 sequence_display != NULL &&
                 ubuf_block_equal(sequence_display,
                                  upipe_mpgvf->sequence_display))))) {
        /* identical sequence header, extension and display, but we rotate them
         * to free older buffers */
        ubuf_free(upipe_mpgvf->sequence_header);
        if (upipe_mpgvf->sequence_ext != NULL)
            ubuf_free(upipe_mpgvf->sequence_ext);
        if (upipe_mpgvf->sequence_display != NULL)
            ubuf_free(upipe_mpgvf->sequence_display);
        upipe_mpgvf->sequence_header = sequence_header;
        upipe_mpgvf->sequence_ext = sequence_ext;
        upipe_mpgvf->sequence_display = sequence_display;
        return true;
    }

    if (upipe_mpgvf->sequence_header != NULL)
        ubuf_free(upipe_mpgvf->sequence_header);
    if (upipe_mpgvf->sequence_ext != NULL)
        ubuf_free(upipe_mpgvf->sequence_ext);
    if (upipe_mpgvf->sequence_display != NULL)
        ubuf_free(upipe_mpgvf->sequence_display);
    upipe_mpgvf->sequence_header = sequence_header;
    upipe_mpgvf->sequence_ext = sequence_ext;
    upipe_mpgvf->sequence_display = sequence_display;

    return upipe_mpgvf_parse_sequence(upipe);
}

/** @internal @This parses a new picture header, and outputs a flow
 * definition
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame
 * @param duration_p filled with duration
 * @return false in case of error
 */
static bool upipe_mpgvf_parse_picture(struct upipe *upipe, struct uref *uref,
                                      uint64_t *duration_p)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    upipe_mpgvf->closed_gop = false;
    bool brokenlink = false;
    if (upipe_mpgvf->next_frame_gop_offset != -1) {
        uint8_t gop_buffer[MP2VGOP_HEADER_SIZE];
        const uint8_t *gop;
        if (unlikely((gop = uref_block_peek(uref,
                                            upipe_mpgvf->next_frame_gop_offset,
                                            MP2VGOP_HEADER_SIZE,
                                            gop_buffer)) == NULL)) {
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }
        upipe_mpgvf->closed_gop = mp2vgop_get_closedgop(gop);
        brokenlink = mp2vgop_get_brokenlink(gop);
        if (unlikely(!uref_block_peek_unmap(uref,
                                            upipe_mpgvf->next_frame_gop_offset,
                                            gop_buffer, gop))) {
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }
        upipe_mpgvf->last_temporal_reference = -1;
        if (upipe_mpgvf->next_frame_gop_offset)
            uref_block_set_header_size(uref,
                                       upipe_mpgvf->next_frame_gop_offset);
    } else if (upipe_mpgvf->next_frame_offset)
        uref_block_set_header_size(uref, upipe_mpgvf->next_frame_offset);

    if ((brokenlink ||
        (!upipe_mpgvf->closed_gop && upipe_mpgvf->got_discontinuity)) &&
        !uref_flow_set_discontinuity(uref)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    uint8_t picture_buffer[MP2VPIC_HEADER_SIZE];
    const uint8_t *picture;
    if (unlikely((picture = uref_block_peek(uref,
                                            upipe_mpgvf->next_frame_offset,
                                            MP2VPIC_HEADER_SIZE,
                                            picture_buffer)) == NULL)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }
    uint16_t temporalreference = mp2vpic_get_temporalreference(picture);
    uint8_t codingtype = mp2vpic_get_codingtype(picture);
    uint16_t vbvdelay = mp2vpic_get_vbvdelay(picture);
    if (unlikely(!uref_block_peek_unmap(uref, upipe_mpgvf->next_frame_offset,
                                        picture_buffer, picture))) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    uint64_t picture_number = upipe_mpgvf->last_picture_number +
        (temporalreference - upipe_mpgvf->last_temporal_reference);
    if (temporalreference > upipe_mpgvf->last_temporal_reference) {
        upipe_mpgvf->last_temporal_reference = temporalreference;
        upipe_mpgvf->last_picture_number = picture_number;
    }
    if (unlikely(!uref_pic_set_number(uref, picture_number) ||
                 !uref_mpgv_set_type(uref, codingtype) ||
                 (vbvdelay != UINT16_MAX && !uref_clock_set_vbv_delay(uref,
                      (uint64_t)vbvdelay * UCLOCK_FREQ / 90000)))) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    bool ret = true;
    *duration_p = UCLOCK_FREQ * upipe_mpgvf->fps.den / upipe_mpgvf->fps.num;
    if (upipe_mpgvf->next_frame_ext_offset != -1) {
        uint8_t ext_buffer[MP2VPICX_HEADER_SIZE];
        const uint8_t *ext;
        if (unlikely((ext = uref_block_peek(uref,
                                            upipe_mpgvf->next_frame_ext_offset,
                                            MP2VPICX_HEADER_SIZE,
                                            ext_buffer)) == NULL)) {
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }
        uint8_t intradc = mp2vpicx_get_intradc(ext);
        uint8_t structure = mp2vpicx_get_structure(ext);
        bool tff = mp2vpicx_get_tff(ext);
        bool rff = mp2vpicx_get_rff(ext);
        bool progressive = mp2vpicx_get_progressive(ext);
        if (unlikely(!uref_block_peek_unmap(uref,
                                            upipe_mpgvf->next_frame_ext_offset,
                                            ext_buffer, ext))) {
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return false;
        }

        if (intradc != 0)
            upipe_warn_va(upipe, "bit depth %"PRIu8" is possibly not supported",
                          intradc + 8);

        if (upipe_mpgvf->progressive_sequence) {
            if (rff)
                *duration_p *= 1 + tff;
        } else {
            if (structure == MP2VPICX_FRAME_PICTURE) {
                if (rff)
                    *duration_p += *duration_p / 2;
            } else
                *duration_p /= 2;
        }

        if (structure & MP2VPICX_TOP_FIELD)
            ret = ret && uref_pic_set_tf(uref);
        if (structure & MP2VPICX_BOTTOM_FIELD)
            ret = ret && uref_pic_set_bf(uref);
        if (tff)
            ret = ret && uref_pic_set_tff(uref);
        if (progressive)
            ret = ret && uref_pic_set_progressive(uref);
    } else {
        ret = ret && uref_pic_set_tf(uref);
        ret = ret && uref_pic_set_bf(uref);
        ret = ret && uref_pic_set_progressive(uref);
    }

    ret = ret && uref_clock_set_duration(uref, *duration_p);
    if (unlikely(!ret)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

    return true;
}

/** @internal @This handles a uref containing a picture header.
 *
 * @param upipe description structure of the pipe
 * @param uref uref containing a frame
 * @param duration_p filled with the duration
 * @return false in case of error
 */
static bool upipe_mpgvf_handle_picture(struct upipe *upipe, struct uref *uref,
                                       uint64_t *duration_p)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    if (unlikely(!upipe_mpgvf_parse_picture(upipe, uref, duration_p)))
        return false;

    uint8_t type;
    if (!uref_mpgv_get_type(uref, &type))
        return false;

    switch (type) {
        case MP2VPIC_TYPE_I: {
            if (upipe_mpgvf->next_frame_sequence)
                uref_flow_set_random(uref);
            else if (upipe_mpgvf->insert_sequence) {
                struct ubuf *ubuf;
                if (upipe_mpgvf->sequence_display != NULL) {
                    ubuf = ubuf_dup(upipe_mpgvf->sequence_display);
                    if (unlikely(ubuf == NULL)) {
                        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
                        return false;
                    }
                    uref_block_insert(uref, 0, ubuf);
                }
                if (upipe_mpgvf->sequence_ext != NULL) {
                    ubuf = ubuf_dup(upipe_mpgvf->sequence_ext);
                    if (unlikely(ubuf == NULL)) {
                        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
                        return false;
                    }
                    uref_block_insert(uref, 0, ubuf);
                }
                ubuf = ubuf_dup(upipe_mpgvf->sequence_header);
                if (unlikely(ubuf == NULL)) {
                    upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
                    return false;
                }
                uref_block_insert(uref, 0, ubuf);
                uref_flow_set_random(uref);
            }

            uint64_t systime_rap = UINT64_MAX;
            uref_clock_get_systime_rap(uref, &systime_rap);
            upipe_mpgvf->systime_rap_ref = upipe_mpgvf->systime_rap;
            upipe_mpgvf->systime_rap = systime_rap;
            break;
        }

        case MP2VPIC_TYPE_P:
            upipe_mpgvf->systime_rap_ref = upipe_mpgvf->systime_rap;
            if (upipe_mpgvf->systime_rap != UINT64_MAX)
                uref_clock_set_systime_rap(uref, upipe_mpgvf->systime_rap);
            break;

        case MP2VPIC_TYPE_B:
            if (upipe_mpgvf->systime_rap_ref != UINT64_MAX)
                uref_clock_set_systime_rap(uref, upipe_mpgvf->systime_rap_ref);
            break;
    }

    if (upipe_mpgvf->closed_gop)
        upipe_mpgvf->systime_rap_ref = upipe_mpgvf->systime_rap;
    return true;
}

/** @internal @This handles and outputs a frame.
 *
 * @param upipe description structure of the pipe
 * @param upump pump that generated the buffer
 * @return false if the stream needs to be resync'd
 */
static bool upipe_mpgvf_output_frame(struct upipe *upipe, struct upump *upump)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    struct uref *uref = NULL;

    /* The PTS can be updated up to the first octet of the picture start code,
     * so any preceding structure must be extracted before, so that the PTS
     * can be properly promoted and taken into account. */
    if (upipe_mpgvf->next_frame_offset) {
        uref = upipe_mpgvf_extract_uref_stream(upipe,
                upipe_mpgvf->next_frame_offset);
        if (unlikely(uref == NULL)) {
            upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
            return true;
        }
    }

#define KEEP_TIMESTAMP(name)                                                \
    uint64_t name = upipe_mpgvf->next_frame_##name;
    KEEP_TIMESTAMP(pts_orig)
    KEEP_TIMESTAMP(pts)
    KEEP_TIMESTAMP(pts_sys)
    KEEP_TIMESTAMP(dts_orig)
    KEEP_TIMESTAMP(dts)
    KEEP_TIMESTAMP(dts_sys)
#undef KEEP_TIMESTAMP
    /* From now on, PTS declaration only impacts the next frame. */
    upipe_mpgvf_flush_pts(upipe);
    upipe_mpgvf_flush_dts(upipe);

    struct uref *uref2 = upipe_mpgvf_extract_uref_stream(upipe,
            upipe_mpgvf->next_frame_size - upipe_mpgvf->next_frame_offset);
    if (unlikely(uref2 == NULL)) {
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return true;
    }
    if (uref != NULL) {
        uref_block_append(uref, uref_detach_ubuf(uref2));
        uref_free(uref2);
    } else
        uref = uref2;

    if (upipe_mpgvf->next_frame_sequence) {
        if (unlikely(!upipe_mpgvf_handle_sequence(upipe, uref))) {
            uref_free(uref);
            return false;
        }
    }

    uint64_t duration;
    if (unlikely(!upipe_mpgvf_handle_picture(upipe, uref, &duration))) {
        uref_free(uref);
        return false;
    }

    bool ret = true;
#define SET_TIMESTAMP(name)                                                 \
    if (name != UINT64_MAX)                                                 \
        ret = ret && uref_clock_set_##name(uref, name);                     \
    else                                                                    \
        uref_clock_delete_##name(uref);
    SET_TIMESTAMP(pts_orig)
    SET_TIMESTAMP(pts)
    SET_TIMESTAMP(pts_sys)
    SET_TIMESTAMP(dts_orig)
    SET_TIMESTAMP(dts)
    SET_TIMESTAMP(dts_sys)
#undef SET_TIMESTAMP

    if (unlikely(!ret)) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UPROBE_ERR_ALLOC);
        return false;
    }

#define INCREMENT_DTS(name)                                                 \
    if (upipe_mpgvf->next_frame_##name == UINT64_MAX && name != UINT64_MAX) \
        upipe_mpgvf->next_frame_##name = name + duration;
    INCREMENT_DTS(dts_orig)
    INCREMENT_DTS(dts)
    INCREMENT_DTS(dts_sys)
#undef INCREMENT_DTS

    upipe_mpgvf_output(upipe, uref, upump);
    return true;
}

/** @internal @This is called back by @ref upipe_mpgvf_append_uref_stream
 * whenever a new uref is promoted in next_uref.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mpgvf_promote_uref(struct upipe *upipe)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    uint64_t ts;
#define SET_TIMESTAMP(name)                                                 \
    if (uref_clock_get_##name(upipe_mpgvf->next_uref, &ts))                 \
        upipe_mpgvf->next_frame_##name = ts;
    SET_TIMESTAMP(pts_orig)
    SET_TIMESTAMP(pts)
    SET_TIMESTAMP(pts_sys)
    SET_TIMESTAMP(dts_orig)
    SET_TIMESTAMP(dts)
    SET_TIMESTAMP(dts_sys)
#undef SET_TIMESTAMP
}

/** @internal @This resets the internal parsing state.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mpgvf_reset(struct upipe *upipe)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    upipe_mpgvf->next_frame_sequence = false;
    upipe_mpgvf->next_frame_sequence_ext_offset = -1;
    upipe_mpgvf->next_frame_sequence_display_offset = -1;
    upipe_mpgvf->next_frame_gop_offset = -1;
    upipe_mpgvf->next_frame_offset = -1;
    upipe_mpgvf->next_frame_ext_offset = -1;
    upipe_mpgvf->next_frame_slice = false;
}

/** @internal @This tries to output frames from the queue of input buffers.
 *
 * @param upipe description structure of the pipe
 * @param upump pump that generated the buffer
 */
static void upipe_mpgvf_work(struct upipe *upipe, struct upump *upump)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    while (upipe_mpgvf->next_uref != NULL) {
        uint8_t start, next;
        if (!upipe_mpgvf_find(upipe, &start, &next))
            return;

        if (unlikely(!upipe_mpgvf->acquired)) {
            upipe_mpgvf_consume_uref_stream(upipe,
                                            upipe_mpgvf->next_frame_size - 4);
            upipe_mpgvf->next_frame_size = 4;

            switch (start) {
                case MP2VPIC_START_CODE:
                    upipe_mpgvf_flush_pts(upipe);
                    upipe_mpgvf_flush_dts(upipe);
                    break;
                case MP2VSEQ_START_CODE:
                    upipe_mpgvf_sync_acquired(upipe);
                    upipe_mpgvf->next_frame_sequence = true;
                    break;
                default:
                    break;
            }
            continue;
        }

        if (unlikely(upipe_mpgvf->next_frame_offset == -1)) {
            if (start == MP2VX_START_CODE) {
                if (mp2vxst_get_id(next) == MP2VX_ID_SEQX)
                    upipe_mpgvf->next_frame_sequence_ext_offset =
                        upipe_mpgvf->next_frame_size - 4;
                else if (mp2vxst_get_id(next) == MP2VX_ID_SEQDX)
                    upipe_mpgvf->next_frame_sequence_display_offset =
                        upipe_mpgvf->next_frame_size - 4;
            } else if (start == MP2VGOP_START_CODE)
                upipe_mpgvf->next_frame_gop_offset =
                    upipe_mpgvf->next_frame_size - 4;
            else if (start == MP2VPIC_START_CODE)
                upipe_mpgvf->next_frame_offset =
                    upipe_mpgvf->next_frame_size - 4;
            continue;
        }

        if (start == MP2VX_START_CODE) {
            if (mp2vxst_get_id(next) == MP2VX_ID_PICX)
                upipe_mpgvf->next_frame_ext_offset =
                    upipe_mpgvf->next_frame_size - 4;
            continue;
        }

        if (start == MP2VUSR_START_CODE)
            continue;

        if (start > MP2VPIC_START_CODE && start <= MP2VPIC_LAST_CODE) {
            /* slice header */
            upipe_mpgvf->next_frame_slice = true;
            continue;
        }

        if (start != MP2VEND_START_CODE)
            upipe_mpgvf->next_frame_size -= 4;

        if (unlikely(!upipe_mpgvf_output_frame(upipe, upump))) {
            upipe_warn(upipe, "erroneous frame headers");
            upipe_mpgvf->next_frame_size = 0;
            upipe_mpgvf->scan_context = UINT32_MAX;
            upipe_mpgvf_sync_lost(upipe);
            upipe_mpgvf_reset(upipe);
            continue;
        }
        upipe_mpgvf_reset(upipe);
        upipe_mpgvf->next_frame_size = 4;

        switch (start) {
            case MP2VSEQ_START_CODE:
                upipe_mpgvf->next_frame_sequence = true;
                break;
            case MP2VGOP_START_CODE:
                upipe_mpgvf->next_frame_gop_offset = 0;
                break;
            case MP2VPIC_START_CODE:
                upipe_mpgvf->next_frame_offset = 0;
                break;
            case MP2VEND_START_CODE:
                upipe_mpgvf->next_frame_size = 0;
                upipe_mpgvf_sync_lost(upipe);
                break;
            default:
                upipe_warn_va(upipe, "erroneous start code %x", start);
                upipe_mpgvf_sync_lost(upipe);
                break;
        }
    }
}

/** @internal @This receives data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump pump that generated the buffer
 */
static void upipe_mpgvf_input(struct upipe *upipe, struct uref *uref,
                              struct upump *upump)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    if (unlikely(uref->ubuf == NULL)) {
        upipe_mpgvf_output(upipe, uref, upump);
        return;
    }

    if (unlikely(uref_flow_get_discontinuity(uref))) {
        if (!upipe_mpgvf->next_frame_slice) {
            /* we do not want discontinuities in the headers before the first
             * slice header; inside the slices it is less destructive */
            upipe_mpgvf_clean_uref_stream(upipe);
            upipe_mpgvf_init_uref_stream(upipe);
            upipe_mpgvf->got_discontinuity = true;
            upipe_mpgvf->next_frame_size = 0;
            upipe_mpgvf->scan_context = UINT32_MAX;
            upipe_mpgvf_sync_lost(upipe);
            upipe_mpgvf_reset(upipe);
        } else
            uref_flow_set_error(upipe_mpgvf->next_uref);
    }

    upipe_mpgvf_append_uref_stream(upipe, uref);
    upipe_mpgvf_work(upipe, upump);
}

/** @This returns the current setting for sequence header insertion.
 *
 * @param upipe description structure of the pipe
 * @param val_p filled with the current setting
 * @return false in case of error
 */
static bool _upipe_mpgvf_get_sequence_insertion(struct upipe *upipe,
                                                int *val_p)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    *val_p = upipe_mpgvf->insert_sequence ? 1 : 0;
    return true;
}

/** @This sets or unsets the sequence header insertion. When true, a sequence
 * headers is inserted in front of every I frame if it is missing, as per
 * ISO-13818-2 specification.
 *
 * @param upipe description structure of the pipe
 * @param val true for sequence header insertion
 * @return false in case of error
 */
static bool _upipe_mpgvf_set_sequence_insertion(struct upipe *upipe,
                                                int val)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    upipe_mpgvf->insert_sequence = !!val;
    return true;
}

/** @internal @This processes control commands on a mpgvf pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_mpgvf_control(struct upipe *upipe,
                                enum upipe_command command, va_list args)
{
    switch (command) {
        case UPIPE_GET_FLOW_DEF: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_mpgvf_get_flow_def(upipe, p);
        }
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_mpgvf_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_mpgvf_set_output(upipe, output);
        }

        case UPIPE_MPGVF_GET_SEQUENCE_INSERTION: {
            unsigned int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_MPGVF_SIGNATURE);
            int *val_p = va_arg(args, int *);
            return _upipe_mpgvf_get_sequence_insertion(upipe, val_p);
        }
        case UPIPE_MPGVF_SET_SEQUENCE_INSERTION: {
            unsigned int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_MPGVF_SIGNATURE);
            int val = va_arg(args, int);
            return _upipe_mpgvf_set_sequence_insertion(upipe, val);
        }
        default:
            return false;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_mpgvf_free(struct upipe *upipe)
{
    struct upipe_mpgvf *upipe_mpgvf = upipe_mpgvf_from_upipe(upipe);
    upipe_throw_dead(upipe);

    upipe_mpgvf_clean_uref_stream(upipe);
    upipe_mpgvf_clean_output(upipe);
    upipe_mpgvf_clean_sync(upipe);

    if (upipe_mpgvf->flow_def_input != NULL)
        uref_free(upipe_mpgvf->flow_def_input);
    if (upipe_mpgvf->sequence_header != NULL)
        ubuf_free(upipe_mpgvf->sequence_header);
    if (upipe_mpgvf->sequence_ext != NULL)
        ubuf_free(upipe_mpgvf->sequence_ext);
    if (upipe_mpgvf->sequence_display != NULL)
        ubuf_free(upipe_mpgvf->sequence_display);

    upipe_mpgvf_free_flow(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_mpgvf_mgr = {
    .signature = UPIPE_MPGVF_SIGNATURE,

    .upipe_alloc = upipe_mpgvf_alloc,
    .upipe_input = upipe_mpgvf_input,
    .upipe_control = upipe_mpgvf_control,
    .upipe_free = upipe_mpgvf_free,

    .upipe_mgr_free = NULL
};

/** @This returns the management structure for all mpgvf pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_mpgvf_mgr_alloc(void)
{
    return &upipe_mpgvf_mgr;
}