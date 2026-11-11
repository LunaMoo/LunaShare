package org.libsdl.app;

import android.content.Context;
import android.text.Editable;
import android.text.Selection;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.RelativeLayout;

import java.nio.charset.StandardCharsets;

/**
 * The on-screen keyboard, talking to the text field as it really is.
 *
 * SDL's own connection keeps a private copy of the text and turns every change the keyboard makes to it into
 * backspaces and typed letters at the end. The text field (Dear ImGui) has its own cursor, emoji, text the app
 * changes - once the two disagreed (a cursor moved by a tap, a long-pressed letter, an emoji turned into a glyph,
 * autocorrect reaching back a word), letters came twice, went missing or the text was wiped.
 *
 * Here the keyboard edits a copy that is always the field's whole text and cursor, and every change is sent over
 * whole (text + cursor): the native side makes the field match. Whatever the field changes on its own (the app,
 * a tap, an emoji) comes back the same way and replaces the copy. Each side's message carries a generation: a
 * keyboard change made on an older copy than the field's latest is dropped instead of being applied over it.
 */
public class LunaIme {
    static final Editable text = Editable.Factory.getInstance().newEditable("");
    static int gen = 0;        // the field's latest state this copy is based on
    static View view;

    /** (native) the text and selection (UTF-8 bytes) of what was sent, the Enter keys pressed after it */
    static native void nativeImeState(int gen, byte[] utf8, int selStart, int selEnd, int enters);

    /** Takes SDL's place for the keyboard: our view (SDL's own one is only made when there's none). */
    public static void install(Context ctx) {
        if (SDLActivity.mTextEdit != null || SDLActivity.mLayout == null) return;
        Edit e = new Edit(ctx);
        SDLActivity.mTextEdit = e;
        view = e;
        SDLActivity.mLayout.addView(e, new RelativeLayout.LayoutParams(1, 1));
        e.setVisibility(View.GONE);
    }

    static int utf16(byte[] b, int bytes) {
        bytes = Math.max(0, Math.min(bytes, b.length));
        return new String(b, 0, bytes, StandardCharsets.UTF_8).length();
    }

    static int utf8(CharSequence s, int chars) {
        chars = Math.max(0, Math.min(chars, s.length()));
        return s.subSequence(0, chars).toString().getBytes(StandardCharsets.UTF_8).length;
    }

    /** (native thread) the field's state: the copy becomes it. restart: the text itself changed / another field */
    public static void setState(final byte[] utf8, final int selStart, final int selEnd, final int newGen, final boolean restart) {
        final View v = view;
        if (v == null) return;
        v.post(() -> {
            gen = newGen;
            String s = new String(utf8, StandardCharsets.UTF_8);
            int a = utf16(utf8, selStart), b = utf16(utf8, selEnd);
            boolean same = s.contentEquals(text);
            BaseInputConnection.removeComposingSpans(text);
            if (!same) text.replace(0, text.length(), s);
            Selection.setSelection(text, Math.min(a, text.length()), Math.min(b, text.length()));
            InputMethodManager imm = (InputMethodManager) v.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm == null) return;
            if (restart || !same) imm.restartInput(v);
            else imm.updateSelection(v, Selection.getSelectionStart(text), Selection.getSelectionEnd(text), -1, -1);
        });
    }

    static class Edit extends DummyEdit {
        Edit(Context c) { super(c); }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            super.onCreateInputConnection(outAttrs);  // (SDL's attributes)
            outAttrs.initialSelStart = Selection.getSelectionStart(text);
            outAttrs.initialSelEnd = Selection.getSelectionEnd(text);
            ic = new Conn(this);
            return ic;
        }
    }

    static class Conn extends BaseInputConnection {
        int batch = 0;
        int enters = 0;
        final View v;

        Conn(View v) { super(v, true); this.v = v; }

        @Override public Editable getEditable() { return text; }

        void changed() {
            if (batch > 0) return;
            int a = Selection.getSelectionStart(text), b = Selection.getSelectionEnd(text);
            if (a < 0) a = b = text.length();
            byte[] u = text.toString().getBytes(StandardCharsets.UTF_8);
            int n = enters;
            enters = 0;
            try { nativeImeState(gen, u, utf8(text, Math.min(a, b)), utf8(text, Math.max(a, b)), n); } catch (Throwable ignored) {}
            InputMethodManager imm = (InputMethodManager) v.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.updateSelection(v, a, b, getComposingSpanStart(text), getComposingSpanEnd(text));
        }

        @Override public boolean beginBatchEdit() { batch++; return true; }
        @Override public boolean endBatchEdit() {
            if (batch > 0 && --batch == 0) changed();
            return batch > 0;
        }

        @Override
        public boolean commitText(CharSequence t, int newCursorPosition) {
            String s = t.toString();
            int nl = 0;
            if (s.indexOf('\n') >= 0) {  // (Enter: a key for the field - sending, not a new line)
                for (int i = 0; i < s.length(); i++) if (s.charAt(i) == '\n') nl++;
                s = s.replace("\n", "");
            }
            boolean r = super.commitText(s, newCursorPosition);
            enters += nl;
            changed();
            return r;
        }

        @Override public boolean setComposingText(CharSequence t, int p) { boolean r = super.setComposingText(t, p); changed(); return r; }
        @Override public boolean setComposingRegion(int a, int b) { boolean r = super.setComposingRegion(a, b); changed(); return r; }
        @Override public boolean finishComposingText() { boolean r = super.finishComposingText(); changed(); return r; }
        @Override public boolean setSelection(int a, int b) { boolean r = super.setSelection(a, b); changed(); return r; }
        @Override public boolean deleteSurroundingText(int a, int b) { boolean r = super.deleteSurroundingText(a, b); changed(); return r; }
        @Override public boolean deleteSurroundingTextInCodePoints(int a, int b) { boolean r = super.deleteSurroundingTextInCodePoints(a, b); changed(); return r; }

        @Override
        public boolean sendKeyEvent(KeyEvent e) {
            int k = e.getKeyCode();
            if (k == KeyEvent.KEYCODE_DEL || k == KeyEvent.KEYCODE_FORWARD_DEL || k == KeyEvent.KEYCODE_ENTER || k == KeyEvent.KEYCODE_NUMPAD_ENTER) {
                if (e.getAction() != KeyEvent.ACTION_DOWN) return true;
                if (k == KeyEvent.KEYCODE_ENTER || k == KeyEvent.KEYCODE_NUMPAD_ENTER) {
                    if (SDLActivity.onNativeSoftReturnKey()) return true;
                    enters++;
                } else {
                    int a = Selection.getSelectionStart(text), b = Selection.getSelectionEnd(text);
                    if (a < 0) a = b = text.length();
                    int lo = Math.min(a, b), hi = Math.max(a, b);
                    if (lo == hi) {
                        if (k == KeyEvent.KEYCODE_DEL && lo > 0) lo = Character.offsetByCodePoints(text, lo, -1);
                        else if (k == KeyEvent.KEYCODE_FORWARD_DEL && hi < text.length()) hi = Character.offsetByCodePoints(text, hi, 1);
                    }
                    if (lo != hi) {
                        removeComposingSpans(text);
                        text.delete(lo, hi);
                    }
                }
                changed();
                return true;
            }
            return super.sendKeyEvent(e);  // (others: SDL's keys - arrows, letters from a hardware keyboard)
        }
    }
}
