package com.vpe.core;

public class NativeBridge {
    public static void cargarMotorFSR3() {
        FSR3NativeBridge.loadNativeEngine();
    }
}