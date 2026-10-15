package com.emu.mali

import android.view.Surface

object NativeBridge {
    init { System.loadLibrary("maliemu") }
    external fun onSurface(surface: Surface): Boolean
    external fun onSurfaceDestroyed()
    external fun startFrameBridge(socketPath: String): Boolean
    external fun launch(box64: String, wine: String, exe: String, workDir: String, log: String)
    external fun stop()
    external fun hasFdm(): Boolean
}
