// Draws emoji with the system emoji font for the native emoji module (liblunashare_emoji.so).
// SPDX-License-Identifier: GPL-3.0-only
package org.lunamoo.lunashare;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Build;

public final class EmojiRenderer {
    private static final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private static final Rect bounds = new Rect();

    private EmojiRenderer() {}

    /** size x size ARGB (not premultiplied) with the emoji centered, or null if the system can't draw it. */
    public static int[] render(String emoji, int size) {
        try {
            if (Build.VERSION.SDK_INT >= 23 && !paint.hasGlyph(emoji)) return null;
            paint.setTextSize(size);
            paint.setColor(0xFFD9D9D9);  // only used by non-colour fallbacks
            paint.getTextBounds(emoji, 0, emoji.length(), bounds);
            int bw = Math.max(1, bounds.width()), bh = Math.max(1, bounds.height());
            float k = size * 0.96f / Math.max(bw, bh);
            Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bmp);
            c.translate(size * 0.5f, size * 0.5f);
            c.scale(k, k);
            c.translate(-bounds.exactCenterX(), -bounds.exactCenterY());
            c.drawText(emoji, 0, 0, paint);
            int[] px = new int[size * size];
            bmp.getPixels(px, 0, size, 0, 0, size, size);
            bmp.recycle();
            return px;
        } catch (Throwable t) {
            return null;
        }
    }
}
