package com.vpe.core;

import java.io.File;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

public class FSR3NativeBridge {

    public static int fgMultiplier = 2;
    private static boolean nativeLoaded = false;

    static {
        loadNativeEngine();
    }

    public static synchronized void loadNativeEngine() {
        if (nativeLoaded) return;

        try {
            // 1. Extrae y carga el motor nativo directamente desde dentro del .jar
            try (InputStream in = FSR3NativeBridge.class.getResourceAsStream("/natives/fsr3_engine.dll")) {
                if (in != null) {
                    Path tempDir = Files.createTempDirectory("vpe_fsr3_native_");
                    File tempDll = tempDir.resolve("fsr3_engine.dll").toFile();
                    tempDll.deleteOnExit();
                    tempDir.toFile().deleteOnExit();

                    Files.copy(in, tempDll.toPath(), StandardCopyOption.REPLACE_EXISTING);
                    System.load(tempDll.getAbsolutePath());
                    nativeLoaded = true;
                    System.out.println("[VPE-FSR3] Motor nativo extraido del JAR y cargado con exito.");
                    return;
                }
            }

            // 2. Respaldo local
            File localDll = new File("fsr3_engine.dll");
            if (localDll.exists()) {
                System.load(localDll.getAbsolutePath());
                nativeLoaded = true;
                return;
            }

            System.err.println("[VPE-FSR3] ERROR: No se encontro /natives/fsr3_engine.dll dentro del JAR.");
        } catch (Throwable t) {
            System.err.println("[VPE-FSR3] Error al cargar la libreria nativa:");
            t.printStackTrace();
        }
    }

    public static native boolean initFSR3(
            long instance, long physDevice, long device, long swapchain,
            int displayWidth, int displayHeight
    );

    public static native boolean dispatchFSR3(
            long cmdBufferPtr, long colorImgPtr, long depthImgPtr, long motionImgPtr, long outputImgPtr,
            int renderWidth, int renderHeight, int displayWidth, int displayHeight,
            float camFlowX, float camFlowY
    );

    public static boolean dispatchFSR3(
            long cmdBufferPtr, long colorImgPtr, long depthImgPtr, long motionImgPtr, long outputImgPtr,
            int renderWidth, int renderHeight, int displayWidth, int displayHeight
    ) {
        return dispatchFSR3(
                cmdBufferPtr, colorImgPtr, depthImgPtr, motionImgPtr, outputImgPtr,
                renderWidth, renderHeight, displayWidth, displayHeight,
                0.0f, 0.0f
        );
    }

    public static native void presentFSR3(
            long queuePtr, long swapchainPtr, int imageIndex, long semaphorePtr,
            int displayWidth, int displayHeight, boolean enableFrameGen, int fgMultiplier
    );

    public static void presentFSR3(
            long queuePtr, long swapchainPtr, int imageIndex, long semaphorePtr,
            int displayWidth, int displayHeight, boolean enableFrameGen
    ) {
        presentFSR3(
                queuePtr, swapchainPtr, imageIndex, semaphorePtr,
                displayWidth, displayHeight, enableFrameGen, fgMultiplier
        );
    }
}