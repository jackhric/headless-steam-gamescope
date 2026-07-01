#pragma once
#include <boost/endian.hpp>
#include <cmath>
#include <gst-plugin/gstrtpmoonlightpay_video.hpp>
#include <gst-plugin/utils.hpp>
#include <helpers/logger.hpp>
#include <moonlight/data-structures.hpp>

namespace gst_moonlight_video {

struct VideoRTPHeaders {
  moonlight::RTP_PACKET rtp;
  char reserved[4];
  moonlight::NV_VIDEO_PACKET packet;
};

#pragma pack(push, 1)
struct VideoShortHeader {
  uint8_t header_type; // Always 0x01 for short headers
  uint8_t unknown[2];
  // Currently known values:
  // 1 = Normal P-frame
  // 2 = IDR-frame
  // 4 = P-frame with intra-refresh blocks
  // 5 = P-frame after reference frame invalidation
  uint8_t frame_type;

  // Length of the final packet payload for codecs that cannot handle
  // zero padding, such as AV1 (Sunshine extension).
  boost::endian::little_uint16_at last_payload_len;

  uint8_t unknown2[2];
};
#pragma pack(pop)

static GstBuffer *
create_rtp_header(const gst_rtp_moonlight_pay_video &rtpmoonlightpay, int packet_nr, int tot_packets) {
  constexpr auto rtp_header_size = sizeof(VideoRTPHeaders);
  GstBuffer *buf = gst_buffer_new_and_fill(rtp_header_size, 0x00);

  GstMapInfo info;
  gst_buffer_map(buf, &info, GST_MAP_WRITE);

  auto packet = (VideoRTPHeaders *)info.data;

  packet->rtp.header = 0x80 | FLAG_EXTENSION;
  packet->rtp.packetType = 0x00;
  packet->rtp.timestamp = 0x00;
  packet->rtp.ssrc = 0x00;

  uint32_t sequence_number = rtpmoonlightpay.cur_seq_number + packet_nr;
  packet->rtp.sequenceNumber = boost::endian::native_to_big((uint16_t)sequence_number);

  packet->packet.frameIndex = rtpmoonlightpay.frame_num;
  packet->packet.streamPacketIndex = sequence_number << 8;

  packet->packet.multiFecFlags = 0x10;
  packet->packet.multiFecBlocks = 0;
  packet->packet.fecInfo = (packet_nr << 12 | tot_packets << 22 | 0 << 4);

  packet->packet.flags = FLAG_CONTAINS_PIC_DATA;

  if (packet_nr == 0) {
    packet->packet.flags |= FLAG_SOF;
  }

  if (packet_nr == tot_packets - 1) {
    packet->packet.flags |= FLAG_EOF;
  }

  gst_buffer_unmap(buf, &info);

  return buf;
}

static GstBuffer *prepend_video_header(const gst_rtp_moonlight_pay_video &rtpmoonlightpay, GstBuffer *inbuf) {
  constexpr auto video_payload_header_size = 8;
  auto in_buf_size = gst_buffer_get_size(inbuf);
  GstBuffer *video_header = gst_buffer_new_and_fill(video_payload_header_size, 0x00);
  bool is_key = !GST_BUFFER_FLAG_IS_SET(inbuf, GST_BUFFER_FLAG_DELTA_UNIT);

  if (is_key) {
    logs::log(logs::trace, "[GStreamer] KEYFRAME!");
  }

  GstMapInfo info;
  gst_buffer_map(video_header, &info, GST_MAP_WRITE);

  auto packet = (VideoShortHeader *)info.data;
  packet->header_type = 0x01;
  packet->frame_type = is_key ? 0x02 : 0x01;
  packet->last_payload_len = (in_buf_size + video_payload_header_size) %
                             (rtpmoonlightpay.payload_size - sizeof(moonlight::NV_VIDEO_PACKET));
  if (packet->last_payload_len == 0) {
    packet->last_payload_len = rtpmoonlightpay.payload_size - sizeof(moonlight::NV_VIDEO_PACKET);
  }

  gst_buffer_unmap(video_header, &info);

  auto full_payload_buf = gst_buffer_append(video_header, gst_buffer_ref(inbuf));
  return full_payload_buf;
}

static GstBufferList *generate_rtp_packets(const gst_rtp_moonlight_pay_video &rtpmoonlightpay, GstBuffer *inbuf) {
  auto in_buf_size = gst_buffer_get_size(inbuf);
  auto payload_size = rtpmoonlightpay.payload_size - MAX_RTP_HEADER_SIZE;
  auto tot_packets = std::ceil((float)in_buf_size / payload_size);
  GstBufferList *buffers = gst_buffer_list_new();

  for (int packet_nr = 0; packet_nr < tot_packets; packet_nr++) {
    auto begin = packet_nr * payload_size;
    auto remaining = in_buf_size - begin;
    auto packet_payload_size = MIN(remaining, payload_size);

    GstBuffer *rtp_packet = create_rtp_header(rtpmoonlightpay, packet_nr, tot_packets);

    GstBuffer *payload = gst_buffer_copy_region(inbuf, GST_BUFFER_COPY_ALL, begin, packet_payload_size);
    rtp_packet = gst_buffer_append(rtp_packet, payload);

    if ((remaining < payload_size) && rtpmoonlightpay.add_padding) {
      GstBuffer *padding = gst_buffer_new_and_fill(payload_size - remaining, 0x00);
      rtp_packet = gst_buffer_append(rtp_packet, padding);
    }

    gst_copy_timestamps(inbuf, rtp_packet);
    gst_buffer_list_add(buffers, rtp_packet);
  }

  return buffers;
}

static void update_fec_info(const gst_rtp_moonlight_pay_video &rtpmoonlightpay,
                            VideoRTPHeaders *rtp_packet,
                            int shard_idx,
                            int data_shards,
                            int fec_percentage,
                            int block_index = 0,
                            int last_block_index = 0) {
  rtp_packet->packet.frameIndex = rtpmoonlightpay.frame_num;

  rtp_packet->packet.fecInfo = (shard_idx << 12 | data_shards << 22 | fec_percentage << 4);
  rtp_packet->packet.multiFecBlocks = (block_index << 4) | last_block_index;
  rtp_packet->packet.multiFecFlags = 0x10;

  rtp_packet->rtp.header = 0x80 | FLAG_EXTENSION;
  uint32_t sequence_number = rtpmoonlightpay.cur_seq_number + shard_idx;
  rtp_packet->rtp.sequenceNumber = boost::endian::native_to_big((uint16_t)sequence_number);
}

struct BLOCKS {
  int block_size;
  int data_shards;
  int parity_shards;
  int fec_percentage;
};

static BLOCKS determine_split(const gst_rtp_moonlight_pay_video &rtpmoonlightpay, int data_shards) {
  auto blocksize = rtpmoonlightpay.payload_size + (int)sizeof(VideoRTPHeaders) - MAX_RTP_HEADER_SIZE;
  auto fec_percentage = rtpmoonlightpay.fec_percentage;
  int parity_shards = (data_shards * fec_percentage + 99) / 100;

  // bump fec_percentage up to meet the min required packets
  if (parity_shards < rtpmoonlightpay.min_required_fec_packets) {
    parity_shards = rtpmoonlightpay.min_required_fec_packets;
    fec_percentage = (100 * parity_shards) / data_shards;
  }

  return {.block_size = blocksize,
          .data_shards = data_shards,
          .parity_shards = parity_shards,
          .fec_percentage = fec_percentage};
}

// Modifies rtp_packets in place with FEC info and appends the FEC packets.
static void generate_fec_packets(const gst_rtp_moonlight_pay_video &rtpmoonlightpay,
                                 GstBufferList *rtp_packets,
                                 GstBuffer *inbuf,
                                 int block_index = 0,
                                 int last_block_index = 0) {
  GstMapInfo info;
  GstBuffer *rtp_payload = gst_buffer_list_unfold(rtp_packets);

  auto payload_size = (int)gst_buffer_get_size(rtp_payload);
  auto blocks = determine_split(rtpmoonlightpay, gst_buffer_list_length(rtp_packets));
  const auto nr_shards = blocks.data_shards + blocks.parity_shards;

  if (nr_shards > DATA_SHARDS_MAX) {
    logs::log(logs::warning,
              "[GSTREAMER] Size of frame too large, {} packets is bigger than the max ({}); skipping FEC",
              nr_shards,
              DATA_SHARDS_MAX);
    gst_buffer_unref(rtp_payload);
    return;
  }

  if (payload_size % blocks.block_size != 0) {
    GstBuffer *pad = gst_buffer_new_and_fill((blocks.data_shards * blocks.block_size) - payload_size, 0x00);
    rtp_payload = gst_buffer_append(rtp_payload, pad);
  }

  auto fec_buff = gst_buffer_new_and_fill((blocks.parity_shards * blocks.block_size), 0x00);
  rtp_payload = gst_buffer_append(rtp_payload, fec_buff);
  gst_buffer_map(rtp_payload, &info, GST_MAP_WRITE);

  auto rs = moonlight::fec::create(blocks.data_shards, blocks.parity_shards);
  std::vector<unsigned char *> ptr(nr_shards);
  for (int shard_idx = 0; shard_idx < nr_shards; shard_idx++) {
    ptr[shard_idx] = info.data + (shard_idx * blocks.block_size);
  }
  if (moonlight::fec::encode(rs.get(), &ptr.front(), nr_shards, blocks.block_size) != 0) {
    logs::log(logs::warning, "Error during video FEC encoding");
  }

  for (int shard_idx = 0; shard_idx < blocks.data_shards; shard_idx++) {
    GstMapInfo data_info;
    auto data_pkt = gst_buffer_list_get(rtp_packets, shard_idx);
    gst_buffer_map(data_pkt, &data_info, GST_MAP_WRITE);

    update_fec_info(rtpmoonlightpay,
                    (VideoRTPHeaders *)(data_info.data),
                    shard_idx,
                    blocks.data_shards,
                    blocks.fec_percentage,
                    block_index,
                    last_block_index);
    gst_copy_timestamps(inbuf, data_pkt);
    gst_buffer_unmap(data_pkt, &data_info);
  }

  for (int shard_idx = blocks.data_shards; shard_idx < nr_shards; shard_idx++) {
    auto position = shard_idx * blocks.block_size;
    auto rtp_packet = (VideoRTPHeaders *)(info.data + position);

    update_fec_info(rtpmoonlightpay,
                    rtp_packet,
                    shard_idx,
                    blocks.data_shards,
                    blocks.fec_percentage,
                    block_index,
                    last_block_index);

    GstBuffer *packet_buf = gst_buffer_new_allocate(nullptr, blocks.block_size, nullptr);
    gst_buffer_fill(packet_buf, 0, rtp_packet, blocks.block_size);
    gst_copy_timestamps(inbuf, packet_buf);
    gst_buffer_list_add(rtp_packets, packet_buf);
  }

  gst_buffer_unmap(rtp_payload, &info);
  gst_buffer_unref(rtp_payload);
}

// Splits the packets into 3 FEC blocks; returns a new linear list, modifying rtp_packets in place.
static GstBufferList *generate_fec_multi_blocks(gst_rtp_moonlight_pay_video *rtpmoonlightpay,
                                                GstBufferList *rtp_packets,
                                                int data_shards,
                                                GstBuffer *inbuf) {
  auto rtp_packets_size = gst_buffer_list_length(rtp_packets);

  constexpr auto nr_blocks = 3;
  constexpr auto last_block_index = 2 << 6;

  GstBufferList *final_packets = gst_buffer_list_new();

  auto packets_per_block = (int)std::ceil((float)data_shards / nr_blocks);
  for (int block_idx = 0; block_idx < nr_blocks; block_idx++) {
    auto list_start = block_idx * packets_per_block;
    auto list_end = MIN((block_idx + 1) * packets_per_block, rtp_packets_size);
    auto block_packets = gst_buffer_list_sub(rtp_packets, list_start, list_end);

    // no data copy: this also mutates FEC info in the original rtp_packets list
    generate_fec_packets(*rtpmoonlightpay, block_packets, inbuf, block_idx, last_block_index);

    auto total_block_packets = gst_buffer_list_length(block_packets);
    for (int packet_idx = 0; packet_idx < total_block_packets; packet_idx++) {
      // copy here is about the buffer object, not the data
      gst_buffer_list_add(final_packets, gst_buffer_copy(gst_buffer_list_get(block_packets, packet_idx)));
    }

    rtpmoonlightpay->cur_seq_number += total_block_packets;
    gst_buffer_list_unref(block_packets);
  }

  gst_buffer_list_unref(rtp_packets);

  return final_packets;
}

static GstBufferList *split_into_rtp(gst_rtp_moonlight_pay_video *rtpmoonlightpay, GstBuffer *inbuf) {
  auto full_payload_buf = prepend_video_header(*rtpmoonlightpay, inbuf);

  GstBufferList *rtp_packets = generate_rtp_packets(*rtpmoonlightpay, full_payload_buf);

  if (rtpmoonlightpay->fec_percentage > 0) {
    auto rtp_packets_size = gst_buffer_list_length(rtp_packets);
    auto blocks = determine_split(*rtpmoonlightpay, rtp_packets_size);

    // fec_percentage=255 with >100 data_shards overflows DATA_SHARDS_MAX and fails to encode.
    if (blocks.data_shards > 90) {
      rtp_packets = generate_fec_multi_blocks(rtpmoonlightpay, rtp_packets, blocks.data_shards, inbuf);
    } else {
      generate_fec_packets(*rtpmoonlightpay, rtp_packets, inbuf, 0, 0);
      rtpmoonlightpay->cur_seq_number += gst_buffer_list_length(rtp_packets);
    }
  }

  rtpmoonlightpay->frame_num++;
  gst_buffer_unref(full_payload_buf);
  return rtp_packets;
}

} // namespace gst_moonlight_video