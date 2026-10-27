package com.vpe.mixin.vulkan;

import com.vpe.core.FSR3NativeBridge;
import com.vpe.core.VPEVulkanClient;
import com.vpe.core.VulkanExtractor;
import net.minecraft.client.MinecraftClient;
import net.minecraft.entity.Entity;
import net.vulkanmod.vulkan.Renderer;
import net.vulkanmod.vulkan.device.DeviceManager;
import net.vulkanmod.vulkan.framebuffer.SwapChain;
import net.vulkanmod.vulkan.texture.VulkanImage;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

@Mixin(value = Renderer.class, remap = false)
public class RendererMixin {

    @Shadow private SwapChain swapChain;
    @Shadow private static int currentFrame;
    @Shadow private int framesNum;
    @Shadow private static int imageIndex;
    @Shadow private ArrayList<Long> renderFinishedSemaphores;

    private static long cachedQueuePtr = 0;
    private static boolean autoInitialized = false;
    private static int lastSwapchainWidth = 0;
    private static int lastSwapchainHeight = 0;
    private static long lastSwapchainId = 0;
    private static Method stackPopMethod = null;
    private static Field mcFramesField = null;
    private static boolean mcFramesFieldSearched = false;

    private static float lastYaw = 0.0f;
    private static float lastPitch = 0.0f;
    private static double lastCamX = 0.0;
    private static double lastCamY = 0.0;
    private static double lastCamZ = 0.0;
    private static float smoothFlowX = 0.0f;
    private static float smoothFlowY = 0.0f;
    private static boolean hasLastCamera = false;

    private static void popLwjglMemoryStack() {
        try {
            if (stackPopMethod == null) {
                Class<?> memoryStackClass = Class.forName("org.lwjgl.system.MemoryStack");
                stackPopMethod = memoryStackClass.getMethod("stackPop");
            }
            stackPopMethod.invoke(null);
        } catch (Throwable ignored) {}
    }

    // Sincroniza el contador F3 de Minecraft con los fotogramas reales generados en Vulkan
    private static void addGeneratedFramesToF3Counter(MinecraftClient mc, int extraFrames) {
        if (mc == null || extraFrames <= 0) return;
        try {
            if (!mcFramesFieldSearched) {
                mcFramesFieldSearched = true;
                for (Field f : MinecraftClient.class.getDeclaredFields()) {
                    if (f.getType() == int.class && (f.getName().equals("frames") || f.getName().equals("field_1738"))) {
                        f.setAccessible(true);
                        mcFramesField = f;
                        break;
                    }
                }
            }
            if (mcFramesField != null) {
                int current = mcFramesField.getInt(mc);
                mcFramesField.setInt(mc, current + extraFrames);
            }
        } catch (Throwable ignored) {}
    }

    private static long resolveQueueAddress() {
        if (cachedQueuePtr != 0) return cachedQueuePtr;
        try {
            Object queueObj = DeviceManager.getPresentQueue();
            if (queueObj == null) queueObj = DeviceManager.getGraphicsQueue();
            if (queueObj == null) return 0;

            Object vkQueueInstance = null;
            Class<?> curr = queueObj.getClass();
            while (curr != null && vkQueueInstance == null) {
                for (Method m : curr.getDeclaredMethods()) {
                    if (m.getName().equals("queue") && m.getParameterCount() == 0) {
                        m.setAccessible(true);
                        vkQueueInstance = m.invoke(queueObj);
                        break;
                    }
                }
                curr = curr.getSuperclass();
            }

            curr = queueObj.getClass();
            while (curr != null && vkQueueInstance == null) {
                for (Field f : curr.getDeclaredFields()) {
                    if (f.getName().equals("queue") || f.getType().getSimpleName().equals("VkQueue")) {
                        f.setAccessible(true);
                        vkQueueInstance = f.get(queueObj);
                        break;
                    }
                }
                curr = curr.getSuperclass();
            }

            if (vkQueueInstance != null) {
                Method addressMethod = vkQueueInstance.getClass().getMethod("address");
                cachedQueuePtr = (long) addressMethod.invoke(vkQueueInstance);
                return cachedQueuePtr;
            }
        } catch (Throwable t) {
            t.printStackTrace();
        }
        return 0;
    }

    private long resolveCurrentSwapchainVkImage() {
        try {
            for (Field f : this.swapChain.getClass().getDeclaredFields()) {
                if (List.class.isAssignableFrom(f.getType())) {
                    f.setAccessible(true);
                    List<?> list = (List<?>) f.get(this.swapChain);
                    if (list != null && imageIndex >= 0 && imageIndex < list.size()) {
                        Object elem = list.get(imageIndex);
                        if (elem instanceof VulkanImage) {
                            return ((VulkanImage) elem).getId();
                        } else if (elem instanceof Long) {
                            return (Long) elem;
                        }
                    }
                }
            }
            VulkanImage colorImg = this.swapChain.getColorAttachment();
            if (colorImg != null) {
                return colorImg.getId();
            }
        } catch (Throwable ignored) {}
        return 0;
    }

    @Inject(
            method = "submitFrame",
            at = @At(
                    value = "INVOKE",
                    target = "Lorg/lwjgl/vulkan/KHRSwapchain;vkQueuePresentKHR(Lorg/lwjgl/vulkan/VkQueue;Lorg/lwjgl/vulkan/VkPresentInfoKHR;)I"
            ),
            cancellable = true
    )
    private void onBeforeQueuePresent(CallbackInfo ci) {
        if (this.swapChain == null) return;

        int displayWidth = this.swapChain.getWidth();
        int displayHeight = this.swapChain.getHeight();
        long swapchainPtr = this.swapChain.getId();

        if (!autoInitialized || swapchainPtr != lastSwapchainId || displayWidth != lastSwapchainWidth || displayHeight != lastSwapchainHeight) {
            try {
                long instancePtr = VulkanExtractor.getVkInstanceAddress();
                long physicalDevicePtr = VulkanExtractor.getVkPhysicalDeviceAddress();
                long devicePtr = VulkanExtractor.getVkDeviceAddress();
                if (instancePtr != 0 && physicalDevicePtr != 0 && devicePtr != 0 && swapchainPtr != 0) {
                    autoInitialized = FSR3NativeBridge.initFSR3(
                            instancePtr, physicalDevicePtr, devicePtr, swapchainPtr, displayWidth, displayHeight
                    );
                    lastSwapchainId = swapchainPtr;
                    lastSwapchainWidth = displayWidth;
                    lastSwapchainHeight = displayHeight;
                }
            } catch (Throwable t) {
                t.printStackTrace();
            }
        }

        if (!VPEVulkanClient.enableFSR && !VPEVulkanClient.enableFrameGen) {
            hasLastCamera = false;
            smoothFlowX = 0.0f;
            smoothFlowY = 0.0f;
            return;
        }

        MinecraftClient mc = MinecraftClient.getInstance();
        boolean inGameWorld = (mc != null && mc.world != null && mc.currentScreen == null);

        float camFlowX = 0.0f;
        float camFlowY = 0.0f;
        if (inGameWorld && mc.getCameraEntity() != null) {
            Entity cam = mc.getCameraEntity();
            float yaw = cam.getYaw();
            float pitch = cam.getPitch();
            double cx = cam.getX();
            double cy = cam.getY();
            double cz = cam.getZ();

            if (hasLastCamera) {
                float dYaw = yaw - lastYaw;
                while (dYaw > 180.0f) dYaw -= 360.0f;
                while (dYaw < -180.0f) dYaw += 360.0f;
                float dPitch = pitch - lastPitch;

                double fovDeg = (mc.options != null) ? mc.options.getFov().getValue() : 70.0;
                double focalLen = (displayHeight * 0.5) / Math.tan(Math.toRadians(Math.max(30.0, fovDeg) * 0.5));
                double cosPitch = Math.max(0.25, Math.cos(Math.toRadians(pitch)));

                float rawFlowX = (float) (-Math.tan(Math.toRadians(dYaw)) * focalLen * cosPitch);
                float rawFlowY = (float) (-Math.tan(Math.toRadians(dPitch)) * focalLen);

                double dx = cx - lastCamX;
                double dy = cy - lastCamY;
                double dz = cz - lastCamZ;
                double yawRad = Math.toRadians(yaw);
                double rightX = -Math.cos(yawRad);
                double rightZ = -Math.sin(yawRad);
                double strafe = dx * rightX + dz * rightZ;

                rawFlowX += (float) (-strafe * (focalLen * 0.22));
                rawFlowY += (float) (dy * (focalLen * 0.22));

                smoothFlowX = smoothFlowX * 0.22f + rawFlowX * 0.78f;
                smoothFlowY = smoothFlowY * 0.22f + rawFlowY * 0.78f;
                camFlowX = smoothFlowX;
                camFlowY = smoothFlowY;
            }

            lastYaw = yaw;
            lastPitch = pitch;
            lastCamX = cx;
            lastCamY = cy;
            lastCamZ = cz;
            hasLastCamera = true;
        } else {
            hasLastCamera = false;
            smoothFlowX = 0.0f;
            smoothFlowY = 0.0f;
        }

        float scale = (VPEVulkanClient.enableFSR && inGameWorld) ? VPEVulkanClient.fsrRenderScale : 1.0f;
        int renderWidth = Math.max(1, (int) (displayWidth * scale));
        int renderHeight = Math.max(1, (int) (displayHeight * scale));

        long colorImgPtr = resolveCurrentSwapchainVkImage();

        FSR3NativeBridge.dispatchFSR3(
                0L, colorImgPtr, 0L, 0L, colorImgPtr,
                renderWidth, renderHeight, displayWidth, displayHeight,
                camFlowX, camFlowY
        );

        long queuePtr = resolveQueueAddress();
        if (queuePtr != 0 && this.renderFinishedSemaphores != null && currentFrame < this.renderFinishedSemaphores.size()) {
            long semaphorePtr = this.renderFinishedSemaphores.get(currentFrame);
            int mult = Math.max(2, Math.min(6, FSR3NativeBridge.fgMultiplier));

            FSR3NativeBridge.presentFSR3(
                    queuePtr,
                    swapchainPtr,
                    imageIndex,
                    semaphorePtr,
                    displayWidth,
                    displayHeight,
                    VPEVulkanClient.enableFrameGen,
                    mult
            );

            if (VPEVulkanClient.enableFrameGen) {
                addGeneratedFramesToF3Counter(mc, mult - 1);
            }

            currentFrame = (currentFrame + 1) % this.framesNum;
            popLwjglMemoryStack();
            ci.cancel();
        }
    }
}