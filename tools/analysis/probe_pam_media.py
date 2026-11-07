"""Bounded PAMF probe and optional first-frame decode using supplied FFmpeg.

No guest execution, media modification, or large temporary copies. Optional
decode reads public AVPacket/AVFrame prefixes verified against FFmpeg n9.0.1
headers, with an exact runtime version gate; format/stream/codec stay opaque.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

# Public prefixes, not guessed offsets. FFmpeg n9.0.1:
# https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.1/libavcodec/packet.h
# https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.1/libavutil/frame.h
# Objects remain allocated/freed exclusively through FFmpeg's APIs.
class PacketPrefix(C.Structure):
    _fields_ = [('buf', C.c_void_p), ('pts', C.c_int64), ('dts', C.c_int64),
                ('data', C.c_void_p), ('size', C.c_int), ('stream_index', C.c_int)]


class Rational(C.Structure):
    _fields_ = [('num', C.c_int), ('den', C.c_int)]


class FramePrefix(C.Structure):
    _fields_ = [('data', C.c_void_p * 8), ('linesize', C.c_int * 8),
                ('extended_data', C.c_void_p), ('width', C.c_int), ('height', C.c_int),
                ('nb_samples', C.c_int), ('format', C.c_int), ('pict_type', C.c_int),
                ('sample_aspect_ratio', Rational), ('pts', C.c_int64)]


def extract_frame(args, util, codec, fmt, context, stream, decoder, versions, version_info):
    expected = dict(avutil=(61 << 16) | (1 << 8) | 101,
                    avcodec=(63 << 16) | (1 << 8) | 101,
                    avformat=(63 << 16) | (1 << 8) | 101)
    if versions != expected or version_info != '9.0.1' or C.sizeof(C.c_void_p) != 8:
        raise ValueError('Frame extraction requires the reviewed 64-bit FFmpeg n9.0.1 ABI')
    ptr = C.c_void_p
    alloc_ctx = bind(codec, 'avcodec_alloc_context3', ptr, ptr)
    open_ctx = bind(codec, 'avcodec_open2', C.c_int, ptr, ptr, ptr)
    free_ctx = bind(codec, 'avcodec_free_context', None, C.POINTER(ptr))
    alloc_packet = bind(codec, 'av_packet_alloc', ptr)
    free_packet = bind(codec, 'av_packet_free', None, C.POINTER(ptr))
    unref_packet = bind(codec, 'av_packet_unref', None, ptr)
    alloc_frame = bind(util, 'av_frame_alloc', ptr)
    free_frame = bind(util, 'av_frame_free', None, C.POINTER(ptr))
    unref_frame = bind(util, 'av_frame_unref', None, ptr)
    read_packet = bind(fmt, 'av_read_frame', C.c_int, ptr, ptr)
    send = bind(codec, 'avcodec_send_packet', C.c_int, ptr, ptr)
    receive = bind(codec, 'avcodec_receive_frame', C.c_int, ptr, ptr)
    pixel_name = bind(util, 'av_get_pix_fmt_name', C.c_char_p, C.c_int)
    image_size = bind(util, 'av_image_get_buffer_size', C.c_int, C.c_int, C.c_int, C.c_int, C.c_int)
    image_copy = bind(util, 'av_image_copy_to_buffer', C.c_int, ptr, C.c_int,
                      C.POINTER(ptr), C.POINTER(C.c_int), C.c_int, C.c_int, C.c_int, C.c_int)
    opt_int = bind(util, 'av_opt_set_int', C.c_int, ptr, C.c_char_p, C.c_int64, C.c_int)
    decoder_ctx, packet, frame = ptr(alloc_ctx(decoder)), ptr(alloc_packet()), ptr(alloc_frame())
    if not decoder_ctx.value or not packet.value or not frame.value:
        raise MemoryError('FFmpeg allocation failed')
    packets, byte_count, frame_count = 0, 0, 0
    output = args.extract_frame.resolve()
    try:
        opt_int(decoder_ctx, b'threads', 1, 0)
        rc = open_ctx(decoder_ctx, decoder, None)
        if rc < 0:
            raise RuntimeError(f'avcodec_open2 failed: {rc}')
        for _ in range(args.max_packets):
            rc = read_packet(context, packet)
            if rc < 0:
                raise RuntimeError(f'No requested frame before av_read_frame returned {rc}')
            packets += 1
            try:
                packet_fields = C.cast(packet, C.POINTER(PacketPrefix)).contents
                if packet_fields.size < 0 or packet_fields.size > 16 * 1024 * 1024:
                    raise ValueError('Unexpected packet size')
                byte_count += packet_fields.size
                if byte_count > 64 * 1024 * 1024:
                    raise ValueError('64 MiB input packet limit reached')
                if packet_fields.stream_index != stream:
                    continue
                rc = send(decoder_ctx, packet)
                if rc < 0:
                    raise RuntimeError(f'avcodec_send_packet failed: {rc}')
                while True:
                    rc = receive(decoder_ctx, frame)
                    if rc == -11:  # AVERROR(EAGAIN), read another compressed packet.
                        break
                    if rc < 0:
                        raise RuntimeError(f'avcodec_receive_frame failed: {rc}')
                    fields = C.cast(frame, C.POINTER(FramePrefix)).contents
                    try:
                        if frame_count != args.frame_index:
                            frame_count += 1
                            continue
                        width, height, pixfmt = fields.width, fields.height, fields.format
                        if not (1 <= width <= 8192 and 1 <= height <= 8192):
                            raise ValueError('Decoded dimensions outside prototype bounds')
                        fmt_name = pixel_name(pixfmt)
                        if not fmt_name or fmt_name != b'yuv420p':
                            raise ValueError(f'Prototype only writes reviewed yuv420p, received {fmt_name}')
                        size = image_size(pixfmt, width, height, 1)
                        if size <= 0 or size > 128 * 1024 * 1024:
                            raise ValueError('Decoded image size outside bounds')
                        buffer = C.create_string_buffer(size)
                        copied = image_copy(buffer, size, fields.data, fields.linesize,
                                            pixfmt, width, height, 1)
                        if copied != size:
                            raise RuntimeError(f'av_image_copy_to_buffer returned {copied}, expected {size}')
                        raw = buffer.raw
                        output.parent.mkdir(parents=True, exist_ok=True)
                        output.write_bytes(raw)
                        report = dict(path=str(output), format=fmt_name.decode(), width=width, height=height,
                                      frame_index=frame_count, pts=fields.pts, picture_type=fields.pict_type,
                                      sample_aspect_ratio=[fields.sample_aspect_ratio.num, fields.sample_aspect_ratio.den],
                                      size=size, sha256=hashlib.sha256(raw).hexdigest(),
                                      packets_read=packets, compressed_bytes=byte_count,
                                      header_tag='n9.0.1',
                                      packet_header='https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.1/libavcodec/packet.h',
                                      frame_header='https://raw.githubusercontent.com/FFmpeg/FFmpeg/n9.0.1/libavutil/frame.h',
                                      runtime_version=version_info, runtime_versions=versions,
                                      source=str(args.media.resolve()), source_modified=False,
                                      game_playback_verified=False)
                        output.with_suffix(output.suffix + '.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
                        return report
                    finally:
                        unref_frame(frame)
            finally:
                unref_packet(packet)
        raise RuntimeError(f'No requested frame within {args.max_packets} packets')
    finally:
        free_frame(C.byref(frame))
        free_packet(C.byref(packet))
        free_ctx(C.byref(decoder_ctx))


def bind(lib, name, result, *params):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, list(params)
    return fn


def worker(args):
    directory = args.dll_dir.resolve()
    media = args.media.resolve()
    dll_search = os.add_dll_directory(str(directory))
    util = C.CDLL(str(directory / 'avutil-61.dll'))
    codec = C.CDLL(str(directory / 'avcodec-63.dll'))
    fmt = C.CDLL(str(directory / 'avformat-63.dll'))
    ptr = C.c_void_p
    versions = {name: bind(lib, name + '_version', C.c_uint)()
                for name, lib in [('avutil', util), ('avcodec', codec), ('avformat', fmt)]}
    version_info = bind(util, 'av_version_info', C.c_char_p)().decode('ascii')
    set_opt = bind(util, 'av_dict_set', C.c_int, C.POINTER(ptr), C.c_char_p, C.c_char_p, C.c_int)
    free_opt = bind(util, 'av_dict_free', None, C.POINTER(ptr))
    error_text = bind(util, 'av_strerror', C.c_int, C.c_int, C.c_char_p, C.c_size_t)
    open_input = bind(fmt, 'avformat_open_input', C.c_int, C.POINTER(ptr), C.c_char_p, ptr, C.POINTER(ptr))
    find_info = bind(fmt, 'avformat_find_stream_info', C.c_int, ptr, ptr)
    find_input_format = bind(fmt, 'av_find_input_format', ptr, C.c_char_p)
    find_best = bind(fmt, 'av_find_best_stream', C.c_int, ptr, C.c_int, C.c_int, C.c_int, C.POINTER(ptr), C.c_int)
    dump = bind(fmt, 'av_dump_format', None, ptr, C.c_int, C.c_char_p, C.c_int)
    close = bind(fmt, 'avformat_close_input', None, C.POINTER(ptr))
    find_decoder = bind(codec, 'avcodec_find_decoder', ptr, C.c_int)
    result = dict(media=str(media), size=media.stat().st_size, dll_dir=str(directory),
                  versions=versions, version_info=version_info,
                  metadata_only=not bool(args.extract_frame), source_modified=False)
    with media.open('rb') as handle:
        header = handle.read(16)
    result['header_hex'] = header.hex()
    if header.startswith(b'PAMF'):
        result['pamf_payload_offset'] = int.from_bytes(header[8:12], 'big') * 2048
    options, context = ptr(), ptr()
    set_opt(C.byref(options), b'probesize', b'8388608', 0)
    set_opt(C.byref(options), b'analyzeduration', b'3000000', 0)
    url = str(media).encode('utf-8')
    try:
        input_format = find_input_format(args.input_format.encode('ascii')) if args.input_format else None
        if args.input_format and not input_format:
            raise ValueError('FFmpeg input format not found: ' + args.input_format)
        result['forced_input_format'] = args.input_format
        rc = open_input(C.byref(context), url, input_format, C.byref(options))
        result['open_result'] = rc
        if rc < 0:
            buf = C.create_string_buffer(256)
            error_text(rc, buf, len(buf))
            result['error'] = buf.value.decode('utf-8', 'replace')
            print(json.dumps(result), flush=True)
            return 1
        rc = find_info(context, None)
        result['stream_info_result'] = rc
        streams = {}
        video_decoder = None
        for kind, typ in [('video', 0), ('audio', 1)]:
            decoder = ptr()
            index = find_best(context, typ, -1, -1, C.byref(decoder), 0)
            row = dict(index=index, decoder_found=bool(decoder.value))
            # AV_CODEC_ID_H264 == 27 in FFmpeg's public codec_id enum. Pointer
            # equality confirms the selected decoder without AVCodec layout.
            if kind == 'video' and decoder.value:
                video_decoder = decoder.value
                row['is_h264_decoder'] = decoder.value == find_decoder(27)
                row['is_mpeg2_decoder'] = decoder.value == find_decoder(2)
            streams[kind] = row
        result['streams'] = streams
        dump(context, 0, url, 0)  # FFmpeg writes authoritative metadata to stderr.
        if args.extract_frame:
            if streams['video']['index'] < 0 or not streams['video'].get('is_mpeg2_decoder'):
                raise ValueError('Extraction prototype requires a detected MPEG-2 video stream')
            result['decoded_frame'] = extract_frame(args, util, codec, fmt, context,
                                                   streams['video']['index'], video_decoder, versions, version_info)
        print(json.dumps(result), flush=True)
        return int(rc < 0)
    finally:
        free_opt(C.byref(options))
        if context.value:
            close(C.byref(context))
        dll_search.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--media', type=Path, default=ROOT / 'native/vfs/PS3_GAME/USRDIR/data/movie/013.pam')
    parser.add_argument('--dll-dir', type=Path, default=ROOT / 'runtime/rpcs3')
    parser.add_argument('--timeout', type=float, default=15)
    parser.add_argument('--input-format', default='mpeg',
                        help='Force demuxer to avoid .pam being misidentified as a PAM still image; empty enables auto probing')
    parser.add_argument('--extract-frame', type=Path, help='Write one decoded frame as packed yuv420p and a JSON sidecar')
    parser.add_argument('--frame-index', type=int, default=0)
    parser.add_argument('--max-packets', type=int, default=256)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not 0 <= args.frame_index <= 300 or not 1 <= args.max_packets <= 2048:
        parser.error('frame-index must be 0..300 and max-packets 1..2048')
    if args.worker:
        return worker(args)
    # A separate child contains FFmpeg faults and analysis time. subprocess.run
    # kills and waits for its child if timeout expires, leaving no probe running.
    command = [sys.executable, '-X', 'utf8', str(Path(__file__).resolve()), '--worker',
               '--media', str(args.media), '--dll-dir', str(args.dll_dir),
               '--input-format', args.input_format]
    if args.extract_frame:
        command += ['--extract-frame', str(args.extract_frame), '--frame-index', str(args.frame_index),
                    '--max-packets', str(args.max_packets)]
    try:
        completed = subprocess.run(command, capture_output=True, timeout=args.timeout)
    except subprocess.TimeoutExpired as exc:
        print(json.dumps(dict(timed_out=True, timeout=args.timeout,
                              worker_terminated=True)), flush=True)
        if exc.stderr:
            sys.stderr.write(exc.stderr.decode('utf-8', 'replace'))
        return 124
    sys.stdout.write(completed.stdout.decode('utf-8', 'replace'))
    sys.stderr.write(completed.stderr.decode('utf-8', 'replace'))
    return completed.returncode


if __name__ == '__main__':
    raise SystemExit(main())
