package com.emu.mali

import android.app.Activity
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager

class MainActivity : Activity(), SurfaceHolder.Callback {
    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val sv = SurfaceView(this)
        sv.holder.addCallback(this)
        setContentView(sv)
    }

    override fun surfaceCreated(h: SurfaceHolder) {
        if (NativeBridge.onSurface(h.surface)) {
            val base = filesDir.absolutePath
            NativeBridge.launch(
                "$base/box64/box64", "$base/wine/bin/wine64",
                "$base/games/game.exe", "$base/games", "$base/emu.log"
            )
        }
    }

    override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {}
    override fun surfaceDestroyed(h: SurfaceHolder) {
        NativeBridge.stop()
        NativeBridge.onSurfaceDestroyed()
    }
}
