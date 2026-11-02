package org.lunamoo.lunashare;

import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.util.Size;
import android.view.Surface;
import android.view.WindowManager;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Video calls (only in the "calls" build): Camera2 feeds the hardware H.264 encoder directly through a Surface, so
 * frames never pass through the CPU; a small second stream gives the self-view. Received video is decoded by the
 * hardware decoder. Driven from native code (liblunashare_video.so, modules/video/video_android.cpp).
 *
 * Control ids match LSV_CTL_* in src/media_api/lunashare_video.h.
 */
public final class VideoEngine {
    static native void nativeEncoded(ByteBuffer buf, int offset, int size, boolean key, int w, int h, int rotation, int codec);
    static native void nativeImage(int source, ByteBuffer y, ByteBuffer u, ByteBuffer v, int yStride, int uvStride,
                                   int uvPixelStride, int w, int h, int rotation, boolean mirror);
    static native void nativeDecoderError();
    static native void nativeCameraError(String msg);
    static native void nativeLog(String msg);

    static final int ZOOM = 1, AUTO_EXPOSURE = 2, EXPOSURE_COMP = 3, ISO = 4, SHUTTER = 5, AUTO_FOCUS = 6, FOCUS = 7,
            TORCH = 8, WHITE_BALANCE = 9, STABILIZATION = 10;
    static final int[] WB_MODES = {CameraMetadata.CONTROL_AWB_MODE_AUTO, CameraMetadata.CONTROL_AWB_MODE_DAYLIGHT,
            CameraMetadata.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT, CameraMetadata.CONTROL_AWB_MODE_SHADE,
            CameraMetadata.CONTROL_AWB_MODE_INCANDESCENT, CameraMetadata.CONTROL_AWB_MODE_FLUORESCENT};

    private static final Object lock = new Object();
    private static HandlerThread camThread, encThread;
    private static Handler camHandler, encHandler;
    private static CameraDevice camera;
    private static CameraCaptureSession session;
    private static CaptureRequest.Builder request;
    private static MediaCodec encoder;
    private static Surface encSurface;
    private static ImageReader preview;
    private static CameraCharacteristics chars;
    private static boolean front;
    private static int sensorOrientation;
    private static int width, height, fps;
    private static int encCodec;  // 0 H.264, 1 HEVC, 2 AV1 (matches lsv::Codec)
    static final String[] MIMES = {MediaFormat.MIMETYPE_VIDEO_AVC, MediaFormat.MIMETYPE_VIDEO_HEVC, "video/av01"};

    /** Codecs with a hardware encoder (bit mask 1 << codec); H.264 always counts. */
    public static int encoderCodecs() { return 1 | (hasCodec(1, true) ? 2 : 0) | (hasCodec(2, true) ? 4 : 0); }

    /** Codecs this phone decodes in hardware (bit mask). */
    public static int decoderCodecs() { return 1 | (hasCodec(1, false) ? 2 : 0) | (hasCodec(2, false) ? 4 : 0); }

    private static boolean hasCodec(int codec, boolean encoder) {
        try {
            for (MediaCodecInfo i : new android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                if (i.isEncoder() != encoder) continue;
                if (Build.VERSION.SDK_INT >= 29 && !i.isHardwareAccelerated()) continue;  // software HEVC/AV1 would drain the battery
                for (String t : i.getSupportedTypes()) if (t.equalsIgnoreCase(MIMES[codec])) return true;
            }
        } catch (Exception ignored) {}
        return false;
    }

    // ---- device position from the motion sensor (works with auto-rotate off / the app locked to portrait)
    private static android.view.OrientationEventListener orient;
    private static volatile int deviceDeg = -1;  // clockwise from the natural (portrait) position, 0/90/180/270

    private static void startOrientation() {
        if (orient != null) return;
        orient = new android.view.OrientationEventListener(ctx()) {
            @Override public void onOrientationChanged(int o) {
                if (o == ORIENTATION_UNKNOWN) return;  // lying flat: keep the last position
                int snapped = ((o + 45) / 90 * 90) % 360;
                // a little hysteresis so it doesn't flicker near 45 degrees
                int d = Math.abs(((o - deviceDeg + 540) % 360) - 180);  // distance to the current position
                if (deviceDeg < 0 || d > 55) deviceDeg = snapped;
            }
        };
        if (orient.canDetectOrientation()) orient.enable();
    }

    private static void stopOrientation() {
        if (orient != null) orient.disable();
        orient = null;
    }
    private static boolean highSpeed;  // > 60 fps: constrained high-speed session (encoder only, no self-view)
    private static String encName = "";
    private static byte[] config;  // SPS/PPS
    private static ByteBuffer keyBuf;
    private static long lastPreview;

    // control state
    private static float zoom = 1f, evComp = 0f, iso = 400f, shutterMs = 10f, focus = 0f;
    private static boolean aeAuto = true, afAuto = true, torch = false, stab = false;
    private static int wb = 0;

    private static Context ctx() { return LunaShareActivity.self.getApplicationContext(); }

    private static CameraManager mgr() { return (CameraManager) ctx().getSystemService(Context.CAMERA_SERVICE); }

    // ------------------------------------------------------------------ cameras
    /** One line per camera: "id\tname\tfacing" (facing: 0 back, 1 front, 2 external). */
    public static String cameras() {
        StringBuilder sb = new StringBuilder();
        try {
            CameraManager m = mgr();
            int backs = 0, fronts = 0;
            for (String id : m.getCameraIdList()) {
                CameraCharacteristics c = m.getCameraCharacteristics(id);
                Integer f = c.get(CameraCharacteristics.LENS_FACING);
                int facing = f == null ? 2 : f == CameraMetadata.LENS_FACING_FRONT ? 1 : f == CameraMetadata.LENS_FACING_BACK ? 0 : 2;
                String name = facing == 1 ? "Front camera" : facing == 0 ? "Back camera" : "External camera";
                int n = facing == 1 ? ++fronts : facing == 0 ? ++backs : 1;
                if (n > 1) name += " " + n;
                float[] fl = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
                if (fl != null && fl.length > 0 && n > 1) name += String.format(java.util.Locale.ROOT, " (%.1f mm)", fl[0]);
                sb.append(id).append('\t').append(name).append('\t').append(facing).append('\n');
            }
        } catch (Exception e) {
            nativeLog("cameras: " + e);
        }
        return sb.toString();
    }

    private static int displayDegrees() {
        try {
            WindowManager wm = LunaShareActivity.self.getWindowManager();
            switch (wm.getDefaultDisplay().getRotation()) {
                case Surface.ROTATION_90: return 90;
                case Surface.ROTATION_180: return 180;
                case Surface.ROTATION_270: return 270;
                default: return 0;
            }
        } catch (Exception e) {
            return 0;
        }
    }

    /** Clockwise rotation that turns the (unmirrored) camera image upright for the current device orientation. */
    private static int rotation() {
        int dev = deviceDeg >= 0 ? deviceDeg : (360 - displayDegrees()) % 360;
        if (front) dev = -dev;
        return ((sensorOrientation + dev) % 360 + 360) % 360;
    }

    private static Size pick(Size[] sizes, int w, int h) {
        Size best = null;
        double bestScore = Double.MAX_VALUE;
        double want = (double) w / h;
        for (Size s : sizes) {
            if (s.getWidth() > 7680 || s.getHeight() > 4352) continue;
            double score = Math.abs(s.getHeight() - h) * 4 + Math.abs(s.getWidth() - w)
                    + Math.abs((double) s.getWidth() / s.getHeight() - want) * 2000 + (s.getHeight() < h ? 300 : 0);
            if (score < bestScore) { bestScore = score; best = s; }
        }
        return best != null ? best : new Size(w, h);
    }

    public static boolean startCamera(String id, int w, int h, int frameRate, int bitrate, int codec) {
        stopCamera();
        encCodec = codec;
        startOrientation();
        try {
            CameraManager m = mgr();
            chars = m.getCameraCharacteristics(id);
            Integer f = chars.get(CameraCharacteristics.LENS_FACING);
            front = f != null && f == CameraMetadata.LENS_FACING_FRONT;
            Integer so = chars.get(CameraCharacteristics.SENSOR_ORIENTATION);
            sensorOrientation = so == null ? 90 : so;
            StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            fps = frameRate;
            highSpeed = false;
        stopOrientation();
            Size size = null;
            if (frameRate > 60) {
                // 120 fps only works in a constrained high-speed session, at the sizes the camera lists for it
                Size[] hs = map.getHighSpeedVideoSizes();
                if (hs != null && hs.length > 0) {
                    Size cand = pick(hs, w, h);
                    for (Range<Integer> r : map.getHighSpeedVideoFpsRangesFor(cand))
                        if (r.getLower() >= frameRate && r.getUpper() >= frameRate && encoderSupports(cand, frameRate)) {
                            size = cand;
                            highSpeed = true;
                            break;
                        }
                }
                if (!highSpeed) {
                    nativeLog("this camera can't do " + frameRate + " fps here; using 60");
                    fps = 60;
                }
            }
            if (size == null) {
                // the biggest size up to the request that the encoder takes at this frame rate (and lower rates if needed)
                Size[] all = map.getOutputSizes(MediaCodec.class);
                Size want = pick(all, w, h);
                for (int tryFps : new int[]{fps, 60, 30, 24}) {
                    if (tryFps > fps) continue;
                    size = largestSupported(all, want, tryFps);
                    if (size != null) { fps = tryFps; break; }
                }
                if (size == null) size = want;
            }
            width = size.getWidth() & ~1;
            height = size.getHeight() & ~1;
            nativeLog("camera " + id + ": " + width + "x" + height + " @ " + fps + " fps" + (highSpeed ? " (high speed)" : ""));

            camThread = new HandlerThread("LunaShare camera");
            camThread.start();
            camHandler = new Handler(camThread.getLooper());
            encThread = new HandlerThread("LunaShare encoder");
            encThread.start();
            encHandler = new Handler(encThread.getLooper());

            if (!openEncoder(bitrate)) { stopCamera(); return false; }

            // the self-view only needs a small picture; ~15 fps of it is plenty (not possible in high-speed sessions)
            if (!highSpeed) {
            Size ps = pick(map.getOutputSizes(ImageFormat.YUV_420_888), 320, 320 * height / Math.max(1, width));
            preview = ImageReader.newInstance(ps.getWidth(), ps.getHeight(), ImageFormat.YUV_420_888, 2);
            preview.setOnImageAvailableListener(r -> {
                Image img = r.acquireLatestImage();
                if (img == null) return;
                long now = System.currentTimeMillis();
                if (now - lastPreview >= 66) {
                    lastPreview = now;
                    sendImage(0, img, rotation(), front);
                }
                img.close();
            }, camHandler);
            }

            final CountDownLatch opened = new CountDownLatch(1);
            final boolean[] ok = {false};
            m.openCamera(id, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) {
                    synchronized (lock) { camera = d; }
                    try {
                        List<Surface> targets = highSpeed ? Arrays.asList(encSurface) : Arrays.asList(encSurface, preview.getSurface());
                        CameraCaptureSession.StateCallback cb = new CameraCaptureSession.StateCallback() {
                            @Override public void onConfigured(CameraCaptureSession s) {
                                synchronized (lock) { session = s; }
                                ok[0] = applyRequest();
                                opened.countDown();
                            }
                            @Override public void onConfigureFailed(CameraCaptureSession s) {
                                nativeLog("capture session failed");
                                opened.countDown();
                            }
                        };
                        if (highSpeed) d.createConstrainedHighSpeedCaptureSession(targets, cb, camHandler);
                        else d.createCaptureSession(targets, cb, camHandler);
                    } catch (Exception e) {
                        nativeLog("session: " + e);
                        opened.countDown();
                    }
                }
                @Override public void onDisconnected(CameraDevice d) {
                    d.close();
                    nativeCameraError("camera disconnected (another app took it?)");
                    opened.countDown();
                }
                @Override public void onError(CameraDevice d, int error) {
                    d.close();
                    nativeCameraError("camera error " + error);
                    opened.countDown();
                }
            }, camHandler);
            if (!opened.await(5, TimeUnit.SECONDS) || !ok[0]) {
                stopCamera();
                return false;
            }
            return true;
        } catch (SecurityException e) {
            nativeLog("no camera permission");
        } catch (Exception e) {
            nativeLog("startCamera: " + e);
        }
        stopCamera();
        return false;
    }

    private static MediaCodecInfo.VideoCapabilities avcCaps() {
        try {
            MediaCodec c = MediaCodec.createEncoderByType(MIMES[encCodec]);
            MediaCodecInfo.VideoCapabilities vc = c.getCodecInfo().getCapabilitiesForType(MIMES[encCodec]).getVideoCapabilities();
            c.release();
            return vc;
        } catch (Exception e) {
            return null;
        }
    }

    private static boolean encoderSupports(Size s, int rate) {
        MediaCodecInfo.VideoCapabilities vc = avcCaps();
        return vc == null || vc.areSizeAndRateSupported(s.getWidth() & ~1, s.getHeight() & ~1, rate);
    }

    /** Largest camera size not bigger than `want` (same aspect preferred) that the encoder handles at `rate`. */
    private static Size largestSupported(Size[] all, Size want, int rate) {
        MediaCodecInfo.VideoCapabilities vc = avcCaps();
        Size best = null;
        double aspect = (double) want.getWidth() / want.getHeight();
        for (Size s : all) {
            if (s.getWidth() > want.getWidth() || s.getHeight() > want.getHeight()) continue;
            if (Math.abs((double) s.getWidth() / s.getHeight() - aspect) > 0.05) continue;
            if (vc != null && !vc.areSizeAndRateSupported(s.getWidth() & ~1, s.getHeight() & ~1, rate)) continue;
            if (best == null || (long) s.getWidth() * s.getHeight() > (long) best.getWidth() * best.getHeight()) best = s;
        }
        return best;
    }

    private static boolean openEncoder(int bitrate) {
        try {
            String mime = MIMES[encCodec];
            MediaFormat fmt = MediaFormat.createVideoFormat(mime, width, height);
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 4);
            if (Build.VERSION.SDK_INT >= 23) {
                fmt.setInteger(MediaFormat.KEY_PRIORITY, 0);  // realtime
                if (fps > 30) fmt.setInteger(MediaFormat.KEY_OPERATING_RATE, fps);  // lets the codec clock up for 60/120 fps
            }
            if (Build.VERSION.SDK_INT >= 29) fmt.setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1);
            encoder = MediaCodec.createEncoderByType(mime);
            MediaCodecInfo info = encoder.getCodecInfo();
            MediaCodecInfo.EncoderCapabilities caps = info.getCapabilitiesForType(mime).getEncoderCapabilities();
            if (caps != null && caps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                fmt.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
            MediaCodecInfo.VideoCapabilities vc = info.getCapabilitiesForType(mime).getVideoCapabilities();
            if (vc != null) fmt.setInteger(MediaFormat.KEY_BIT_RATE, vc.getBitrateRange().clamp(bitrate));
            encName = info.getName() + (Build.VERSION.SDK_INT >= 29 && info.isHardwareAccelerated() ? " (hardware)" : "");
            config = null;
            encoder.setCallback(new MediaCodec.Callback() {
                @Override public void onInputBufferAvailable(MediaCodec c, int i) {}
                @Override public void onOutputBufferAvailable(MediaCodec c, int i, MediaCodec.BufferInfo bi) {
                    try {
                        ByteBuffer b = c.getOutputBuffer(i);
                        if (b != null && bi.size > 0) {
                            if ((bi.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                                config = new byte[bi.size];
                                b.position(bi.offset);
                                b.get(config);
                                // AV1: the config is an av1C box (4 header bytes, then the sequence header OBU)
                                if (encCodec == 2 && config.length > 4 && (config[0] & 0xff) == 0x81)
                                    config = Arrays.copyOfRange(config, 4, config.length);
                            } else {
                                boolean key = (bi.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                                if (key && config != null && !hasSps(b, bi.offset, bi.size)) {
                                    int need = config.length + bi.size;
                                    if (keyBuf == null || keyBuf.capacity() < need) keyBuf = ByteBuffer.allocateDirect(need * 2);
                                    keyBuf.clear();
                                    keyBuf.put(config);
                                    b.position(bi.offset).limit(bi.offset + bi.size);
                                    keyBuf.put(b);
                                    nativeEncoded(keyBuf, 0, need, true, width, height, rotation(), encCodec);
                                } else {
                                    nativeEncoded(b, bi.offset, bi.size, key, width, height, rotation(), encCodec);
                                }
                            }
                        }
                        c.releaseOutputBuffer(i, false);
                    } catch (Exception e) {
                        nativeLog("encoder output: " + e);
                    }
                }
                @Override public void onError(MediaCodec c, MediaCodec.CodecException e) {
                    nativeCameraError("encoder: " + e.getMessage());
                }
                @Override public void onOutputFormatChanged(MediaCodec c, MediaFormat f) {}
            }, encHandler);
            encoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            encSurface = encoder.createInputSurface();
            encoder.start();
            return true;
        } catch (Exception e) {
            nativeLog("encoder: " + e);
            return false;
        }
    }

    /** Does this keyframe already carry the stream headers (SPS / VPS / AV1 sequence header)? */
    private static boolean hasSps(ByteBuffer b, int off, int size) {
        if (encCodec == 2) {
            // AV1: OBUs; a temporal delimiter (type 2) may come first
            int i = off, end = off + size;
            for (int k = 0; k < 3 && i < end; k++) {
                int h = b.get(i) & 0xff, type = (h >> 3) & 0xf;
                if (type == 1) return true;
                boolean ext = (h & 4) != 0, hasSize = (h & 2) != 0;
                int p = i + 1 + (ext ? 1 : 0);
                if (!hasSize) return false;
                long len = 0;
                for (int s = 0; s < 8 && p < end; s++) {
                    int byt = b.get(p++) & 0xff;
                    len |= (long) (byt & 0x7f) << (7 * s);
                    if ((byt & 0x80) == 0) break;
                }
                i = (int) (p + len);
            }
            return false;
        }
        for (int i = off; i < off + Math.min(size, 64) - 4; i++)
            if (b.get(i) == 0 && b.get(i + 1) == 0 && b.get(i + 2) == 1) {
                int x = b.get(i + 3) & 0xff;
                if (encCodec == 1 ? ((x >> 1) & 0x3f) == 32 || ((x >> 1) & 0x3f) == 33 : (x & 0x1f) == 7) return true;
            }
        return false;
    }

    public static void stopCamera() {
        synchronized (lock) {
            try { if (session != null) session.close(); } catch (Exception ignored) {}
            try { if (camera != null) camera.close(); } catch (Exception ignored) {}
            session = null;
            camera = null;
            request = null;
        }
        try { if (encoder != null) { encoder.stop(); encoder.release(); } } catch (Exception ignored) {}
        encoder = null;
        if (encSurface != null) encSurface.release();
        encSurface = null;
        if (preview != null) preview.close();
        preview = null;
        highSpeed = false;
        if (camThread != null) camThread.quitSafely();
        if (encThread != null) encThread.quitSafely();
        camThread = encThread = null;
        torch = false;
    }

    public static void setBitrate(int bps) {
        try {
            if (encoder == null) return;
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps);
            encoder.setParameters(b);
        } catch (Exception ignored) {}
    }

    public static void requestKeyframe() {
        try {
            if (encoder == null) return;
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            encoder.setParameters(b);
        } catch (Exception ignored) {}
    }

    // ------------------------------------------------------------------ controls
    private static Range<Integer> fpsRange() {
        Range<Integer>[] ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        Range<Integer> best = null;
        if (ranges != null)
            for (Range<Integer> r : ranges) {
                if (r.getUpper() < fps) continue;
                // a fixed rate close to what we want saves power; otherwise the narrowest range covering it
                if (best == null || r.getUpper() < best.getUpper()
                        || (r.getUpper().equals(best.getUpper()) && r.getLower() > best.getLower())) best = r;
            }
        return best;
    }

    private static float[] zoomRange() {
        if (Build.VERSION.SDK_INT >= 30) {
            Range<Float> r = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
            if (r != null) return new float[]{r.getLower(), r.getUpper()};
        }
        Float mx = chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
        return new float[]{1f, mx == null ? 1f : mx};
    }

    private static boolean manualSensor() {
        int[] caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        if (caps != null) for (int c : caps) if (c == CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) return true;
        return false;
    }

    private static boolean applyRequest() {
        synchronized (lock) {
            if (camera == null || session == null) return false;
            try {
                if (request == null) {
                    request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                    request.addTarget(encSurface);
                    if (preview != null) request.addTarget(preview.getSurface());
                }
                Range<Integer> fr = highSpeed ? new Range<>(fps, fps) : fpsRange();
                if (fr != null) request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fr);
                // zoom
                float[] zr = zoomRange();
                float z = Math.max(zr[0], Math.min(zr[1], zoom));
                if (Build.VERSION.SDK_INT >= 30 && chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null) {
                    request.set(CaptureRequest.CONTROL_ZOOM_RATIO, z);
                } else {
                    Rect a = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
                    if (a != null) {
                        int cw = (int) (a.width() / Math.max(1f, z)), ch = (int) (a.height() / Math.max(1f, z));
                        int cx = a.centerX(), cy = a.centerY();
                        request.set(CaptureRequest.SCALER_CROP_REGION, new Rect(cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2));
                    }
                }
                // exposure
                if (aeAuto || !manualSensor()) {
                    request.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
                    Rational step = new Rational(chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP));
                    request.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, (int) Math.round(evComp / step.value()));
                } else {
                    request.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                    request.set(CaptureRequest.SENSOR_SENSITIVITY, Math.round(iso));
                    long frameNs = 1000000000L / Math.max(1, fps);
                    request.set(CaptureRequest.SENSOR_EXPOSURE_TIME, Math.min(frameNs, (long) (shutterMs * 1e6)));
                    request.set(CaptureRequest.SENSOR_FRAME_DURATION, frameNs);
                }
                // focus
                if (afAuto) {
                    request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                } else {
                    request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                    request.set(CaptureRequest.LENS_FOCUS_DISTANCE, focus);
                }
                request.set(CaptureRequest.FLASH_MODE, torch ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF);
                request.set(CaptureRequest.CONTROL_AWB_MODE, WB_MODES[Math.max(0, Math.min(WB_MODES.length - 1, wb))]);
                request.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                        stab ? CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON : CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
                if (highSpeed && session instanceof android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession) {
                    // high-speed sessions only accept bursts built by the session, and keep 3A on auto
                    request.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
                    request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                    request.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO);
                    android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession hs =
                            (android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession) session;
                    session.setRepeatingBurst(hs.createHighSpeedRequestList(request.build()), null, camHandler);
                } else {
                    session.setRepeatingRequest(request.build(), null, camHandler);
                }
                return true;
            } catch (Exception e) {
                nativeLog("camera request: " + e);
                return false;
            }
        }
    }

    /** Helper: android.util.Rational without importing it into the method above. */
    private static final class Rational {
        final double v;
        Rational(android.util.Rational r) { v = r == null || r.getDenominator() == 0 ? 1 : r.doubleValue(); }
        double value() { return v == 0 ? 1 : v; }
    }

    /** One line per available control: "id\tmin\tmax\tstep\tvalue". */
    public static String controls() {
        StringBuilder sb = new StringBuilder();
        if (chars == null) return "";
        try {
            float[] zr = zoomRange();
            if (zr[1] > zr[0]) line(sb, ZOOM, zr[0], zr[1], 0.1f, zoom);
            Range<Integer> ev = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);
            double step = new Rational(chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)).value();
            boolean manual = manualSensor() && !highSpeed;
            if (manual) line(sb, AUTO_EXPOSURE, 0, 1, 1, aeAuto ? 1 : 0);
            if (ev != null && ev.getUpper() > ev.getLower())
                line(sb, EXPOSURE_COMP, (float) (ev.getLower() * step), (float) (ev.getUpper() * step), (float) step, evComp);
            if (manual) {
                Range<Integer> isoR = chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
                Range<Long> exR = chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
                if (isoR != null) line(sb, ISO, isoR.getLower(), isoR.getUpper(), 1, iso);
                if (exR != null) {
                    float maxMs = Math.min(exR.getUpper() / 1e6f, 1000f / Math.max(1, fps));
                    line(sb, SHUTTER, exR.getLower() / 1e6f, maxMs, 0, shutterMs);
                }
            }
            Float minFocus = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
            if (minFocus != null && minFocus > 0 && !highSpeed) {
                line(sb, AUTO_FOCUS, 0, 1, 1, afAuto ? 1 : 0);
                line(sb, FOCUS, 0, minFocus, 0, focus);
            }
            Boolean flash = chars.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
            if (flash != null && flash) line(sb, TORCH, 0, 1, 1, torch ? 1 : 0);
            int[] awb = chars.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
            if (awb != null && awb.length > 2 && !highSpeed) line(sb, WHITE_BALANCE, 0, WB_MODES.length - 1, 1, wb);
            int[] st = chars.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES);
            if (st != null) for (int m : st) if (m == CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON) line(sb, STABILIZATION, 0, 1, 1, stab ? 1 : 0);
        } catch (Exception e) {
            nativeLog("controls: " + e);
        }
        return sb.toString();
    }

    private static void line(StringBuilder sb, int id, float mn, float mx, float step, float v) {
        sb.append(id).append('\t').append(mn).append('\t').append(mx).append('\t').append(step).append('\t').append(v).append('\n');
    }

    public static void setControl(int id, float v) {
        switch (id) {
            case ZOOM: zoom = v; break;
            case AUTO_EXPOSURE: aeAuto = v > 0.5f; break;
            case EXPOSURE_COMP: evComp = v; break;
            case ISO: iso = v; aeAuto = false; break;
            case SHUTTER: shutterMs = v; aeAuto = false; break;
            case AUTO_FOCUS: afAuto = v > 0.5f; break;
            case FOCUS: focus = v; afAuto = false; break;
            case TORCH: torch = v > 0.5f; break;
            case WHITE_BALANCE: wb = Math.round(v); break;
            case STABILIZATION: stab = v > 0.5f; break;
            default: return;
        }
        if (camHandler != null) camHandler.post(VideoEngine::applyRequest);
    }

    public static String encoderName() { return encName; }

    // ------------------------------------------------------------------ decoding (called on the native decode thread)
    private static MediaCodec decoder;
    private static int decW, decH;
    private static String decName = "";
    private static final MediaCodec.BufferInfo outInfo = new MediaCodec.BufferInfo();

    public static boolean startDecoder() { return true; }  // created on the first keyframe, when the size is known

    private static int decCodec = -1;

    private static boolean openDecoder(int w, int h, int codec) {
        closeDecoder();
        try {
            MediaFormat fmt = MediaFormat.createVideoFormat(MIMES[codec], w, h);
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            if (Build.VERSION.SDK_INT >= 30) fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            if (Build.VERSION.SDK_INT >= 23) fmt.setInteger(MediaFormat.KEY_PRIORITY, 0);
            decoder = MediaCodec.createDecoderByType(MIMES[codec]);
            decCodec = codec;
            MediaCodecInfo info = decoder.getCodecInfo();
            decName = info.getName() + (Build.VERSION.SDK_INT >= 29 && info.isHardwareAccelerated() ? " (hardware)" : "");
            decoder.configure(fmt, null, null, 0);
            decoder.start();
            decW = w;
            decH = h;
            return true;
        } catch (Exception e) {
            nativeLog("decoder: " + e);
            closeDecoder();
            return false;
        }
    }

    private static void closeDecoder() {
        try { if (decoder != null) { decoder.stop(); decoder.release(); } } catch (Exception ignored) {}
        decoder = null;
    }

    public static void decode(ByteBuffer data, int len, boolean key, int w, int h, int codec) {
        if (decoder == null || w != decW || h != decH || codec != decCodec) {
            if (!key) { nativeDecoderError(); return; }
            if (!openDecoder(w, h, codec)) { nativeDecoderError(); return; }
        }
        try {
            int in = decoder.dequeueInputBuffer(20000);
            if (in < 0) { drain(); return; }  // decoder busy: skip, the next keyframe request recovers
            ByteBuffer ib = decoder.getInputBuffer(in);
            ib.clear();
            data.position(0).limit(len);
            ib.put(data);
            decoder.queueInputBuffer(in, 0, len, System.nanoTime() / 1000, key ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0);
            drain();
        } catch (Exception e) {
            nativeLog("decode: " + e);
            closeDecoder();
            nativeDecoderError();
        }
    }

    private static void drain() {
        for (;;) {
            int out = decoder.dequeueOutputBuffer(outInfo, 5000);
            if (out < 0) return;
            Image img = decoder.getOutputImage(out);
            if (img != null) {
                sendImage(1, img, 0, false);  // the native side knows the sender's rotation
                img.close();
            }
            decoder.releaseOutputBuffer(out, false);
        }
    }

    public static void stopDecoder() { closeDecoder(); }

    public static String decoderName() { return decoder != null ? decName : ""; }

    private static void sendImage(int source, Image img, int rotation, boolean mirror) {
        Image.Plane[] p = img.getPlanes();
        if (p.length < 3) return;
        Rect crop = img.getCropRect();
        int w = crop != null && crop.width() > 0 ? crop.width() : img.getWidth();
        int h = crop != null && crop.height() > 0 ? crop.height() : img.getHeight();
        nativeImage(source, p[0].getBuffer(), p[1].getBuffer(), p[2].getBuffer(), p[0].getRowStride(), p[1].getRowStride(),
                p[1].getPixelStride(), w, h, rotation, mirror);
    }
}
