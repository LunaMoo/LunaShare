package org.lunamoo.lunashare;

import android.graphics.Rect;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaDataSource;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Build;

import java.nio.ByteBuffer;

/**
 * Plays audio/video files shared in chat (all builds): the platform's MediaExtractor + hardware MediaCodec decoders.
 * Driven step by step from native code (liblunashare_player.so, modules/player/player_android.cpp), which receives
 * PCM and YUV pictures through the natives below.
 */
public final class PlayerEngine {
    static native void nativeAudio(long target, ByteBuffer pcm, int offset, int size, int rate, int channels, boolean isFloat, long ptsUs);
    static native void nativeVideo(long target, ByteBuffer y, ByteBuffer u, ByteBuffer v, int yStride, int uvStride, int uvPixelStride,
                                   int w, int h, int rotation, long ptsUs);

    private MediaExtractor ex;
    private MediaCodec vdec, adec;
    private int vt = -1, at = -1, rotation, width, height, rate, channels;
    private boolean pcmFloat;
    private long durationUs, skipUntilUs = -1;
    private boolean inputDone, vDone = true, aDone = true;
    private String vMime = "", aMime = "";
    private final MediaCodec.BufferInfo bi = new MediaCodec.BufferInfo();

    static native int nativeRead(long stream, long position, byte[] buffer, int offset, int size);

    /** A file that is still arriving ("watch together"): reads wait in native code until the bytes are here. */
    static final class StreamSource extends MediaDataSource {
        private final long stream, size;
        StreamSource(long stream, long size) { this.stream = stream; this.size = size; }
        @Override public int readAt(long position, byte[] buffer, int offset, int len) {
            if (position >= size) return -1;
            int n = nativeRead(stream, position, buffer, offset, len);
            return n == 0 ? -1 : n < 0 ? -1 : n;
        }
        @Override public long getSize() { return size; }
        @Override public void close() {}
    }

    public static PlayerEngine open(String path, boolean wantAudio) { return openFrom(path, 0, 0, wantAudio); }
    public static PlayerEngine openStream(long stream, long size, boolean wantAudio) { return openFrom(null, stream, size, wantAudio); }

    private static PlayerEngine openFrom(String path, long stream, long size, boolean wantAudio) {
        PlayerEngine p = new PlayerEngine();
        try {
            p.ex = new MediaExtractor();
            if (path != null) p.ex.setDataSource(path);
            else p.ex.setDataSource(new StreamSource(stream, size));
            for (int i = 0; i < p.ex.getTrackCount(); i++) {
                MediaFormat f = p.ex.getTrackFormat(i);
                String mime = f.getString(MediaFormat.KEY_MIME);
                if (mime == null) continue;
                if (p.vt < 0 && mime.startsWith("video/")) { p.vt = i; p.vMime = mime; }
                else if (p.at < 0 && mime.startsWith("audio/")) { p.at = i; p.aMime = mime; }
            }
            if (p.vt >= 0) {
                MediaFormat f = p.ex.getTrackFormat(p.vt);
                p.width = f.getInteger(MediaFormat.KEY_WIDTH);
                p.height = f.getInteger(MediaFormat.KEY_HEIGHT);
                if (Build.VERSION.SDK_INT >= 23 && f.containsKey(MediaFormat.KEY_ROTATION)) p.rotation = f.getInteger(MediaFormat.KEY_ROTATION);
                if (f.containsKey(MediaFormat.KEY_DURATION)) p.durationUs = Math.max(p.durationUs, f.getLong(MediaFormat.KEY_DURATION));
                f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
                p.vdec = MediaCodec.createDecoderByType(p.vMime);
                p.vdec.configure(f, null, null, 0);
                p.vdec.start();
                p.ex.selectTrack(p.vt);
                p.vDone = false;
            }
            if (p.at >= 0) {
                MediaFormat f = p.ex.getTrackFormat(p.at);
                if (f.containsKey(MediaFormat.KEY_DURATION)) p.durationUs = Math.max(p.durationUs, f.getLong(MediaFormat.KEY_DURATION));
                p.rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                p.channels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                if (wantAudio) {
                    p.adec = MediaCodec.createDecoderByType(p.aMime);
                    p.adec.configure(f, null, null, 0);
                    p.adec.start();
                    p.ex.selectTrack(p.at);
                    p.aDone = false;
                }
            }
            if (p.vdec == null && p.at < 0) { p.close(); return null; }
            return p;
        } catch (Exception e) {
            p.close();
            return null;
        }
    }

    /** "duration_s \t has_video \t has_audio \t width \t height \t rotation \t video codec \t audio codec" (upright size) */
    public String info() {
        boolean swap = rotation == 90 || rotation == 270;
        return (durationUs / 1e6) + "\t" + (vdec != null ? 1 : 0) + "\t" + (at >= 0 ? 1 : 0) + "\t" + (swap ? height : width) + "\t"
                + (swap ? width : height) + "\t" + rotation + "\t" + vMime.replace("video/", "") + "\t" + aMime.replace("audio/", "");
    }

    /** Feed one sample and collect whatever the decoders have ready. 1 = go on, 0 = end, -1 = error. */
    public int step(long target) {
        try {
            if (!inputDone) {
                int track = ex.getSampleTrackIndex();
                if (track < 0) {
                    inputDone = true;
                    queueEos(vdec);
                    queueEos(adec);
                } else {
                    MediaCodec c = track == vt ? vdec : track == at ? adec : null;
                    if (c != null) {
                        int in = c.dequeueInputBuffer(5000);
                        if (in >= 0) {
                            ByteBuffer b = c.getInputBuffer(in);
                            int n = ex.readSampleData(b, 0);
                            if (n < 0) n = 0;
                            c.queueInputBuffer(in, 0, n, ex.getSampleTime(), 0);
                            ex.advance();
                        }
                    } else {
                        ex.advance();
                    }
                }
            }
            drainVideo(target);
            drainAudio(target);
            return vDone && aDone ? 0 : 1;
        } catch (Exception e) {
            return -1;
        }
    }

    private void queueEos(MediaCodec c) {
        if (c == null) return;
        int in = c.dequeueInputBuffer(10000);
        if (in >= 0) c.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
    }

    private void drainVideo(long target) {
        if (vdec == null || vDone) return;
        for (;;) {
            int out = vdec.dequeueOutputBuffer(bi, inputDone ? 10000 : 0);
            if (out < 0) return;
            if ((bi.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) vDone = true;
            if (bi.size > 0 && bi.presentationTimeUs >= skipUntilUs) {
                Image img = vdec.getOutputImage(out);
                if (img != null) {
                    Image.Plane[] p = img.getPlanes();
                    Rect crop = img.getCropRect();
                    int w = crop != null && crop.width() > 0 ? crop.width() : img.getWidth();
                    int h = crop != null && crop.height() > 0 ? crop.height() : img.getHeight();
                    if (p.length >= 3)
                        nativeVideo(target, p[0].getBuffer(), p[1].getBuffer(), p[2].getBuffer(), p[0].getRowStride(), p[1].getRowStride(),
                                p[1].getPixelStride(), w, h, rotation, bi.presentationTimeUs);
                    img.close();
                }
            }
            vdec.releaseOutputBuffer(out, false);
            if (vDone) return;
        }
    }

    private void drainAudio(long target) {
        if (adec == null || aDone) return;
        for (;;) {
            int out = adec.dequeueOutputBuffer(bi, inputDone ? 10000 : 0);
            if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat f = adec.getOutputFormat();
                rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                channels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                pcmFloat = Build.VERSION.SDK_INT >= 24 && f.containsKey(MediaFormat.KEY_PCM_ENCODING)
                        && f.getInteger(MediaFormat.KEY_PCM_ENCODING) == android.media.AudioFormat.ENCODING_PCM_FLOAT;
                continue;
            }
            if (out < 0) return;
            if ((bi.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) aDone = true;
            if (bi.size > 0 && bi.presentationTimeUs >= skipUntilUs) {
                ByteBuffer b = adec.getOutputBuffer(out);
                if (b != null) nativeAudio(target, b, bi.offset, bi.size, rate, channels, pcmFloat, bi.presentationTimeUs);
            }
            adec.releaseOutputBuffer(out, false);
            if (aDone) return;
        }
    }

    public void seek(double seconds) {
        long us = (long) (seconds * 1e6);
        try {
            ex.seekTo(us, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
            if (vdec != null) { vdec.flush(); vDone = false; }
            if (adec != null) { adec.flush(); aDone = false; }
            inputDone = false;
            skipUntilUs = us;  // decode quietly up to the target
        } catch (Exception ignored) {}
    }

    public void close() {
        try { if (vdec != null) { vdec.stop(); vdec.release(); } } catch (Exception ignored) {}
        try { if (adec != null) { adec.stop(); adec.release(); } } catch (Exception ignored) {}
        try { if (ex != null) ex.release(); } catch (Exception ignored) {}
        vdec = adec = null;
        ex = null;
    }
}
