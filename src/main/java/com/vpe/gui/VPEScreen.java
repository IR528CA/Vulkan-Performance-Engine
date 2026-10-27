package com.vpe.gui;

import net.minecraft.client.gui.screen.Screen;
import net.minecraft.client.gui.widget.ButtonWidget;
import net.minecraft.client.gui.DrawContext;
import net.minecraft.text.Text;
import com.vpe.core.VPEVulkanClient;
import com.vpe.core.VulkanExtractor;
import com.vpe.core.FSR3NativeBridge;

public class VPEScreen extends Screen {
    private final Screen parent;

    public VPEScreen(Screen parent) {
        super(Text.literal("VPE: Native Vulkan Control"));
        this.parent = parent;
    }

    private static String getFsrPresetName(float scale) {
        if (scale >= 0.99f) return "Nativo AA (1.00x)";
        if (scale >= 0.75f) return "Ultra Calidad (0.77x)";
        if (scale >= 0.65f) return "Calidad (0.67x)";
        if (scale >= 0.57f) return "Equilibrado (0.59x)";
        return "Rendimiento (0.50x)";
    }

    private static float getNextFsrPresetScale(float current) {
        if (current >= 0.99f) return 0.77f;
        if (current >= 0.75f) return 0.67f;
        if (current >= 0.65f) return 0.59f;
        if (current >= 0.57f) return 0.50f;
        return 1.00f;
    }

    @Override
    protected void init() {
        int startY = this.height / 4;

        this.addDrawableChild(ButtonWidget.builder(
                Text.literal("AMD FSR 3 (Upscaler + RCAS): " + (VPEVulkanClient.enableFSR ? "ON" : "OFF")),
                button -> {
                    VPEVulkanClient.enableFSR = !VPEVulkanClient.enableFSR;
                    this.client.setScreen(new VPEScreen(this.parent));
                }).dimensions(this.width / 2 - 125, startY, 250, 20).build());

        this.addDrawableChild(ButtonWidget.builder(
                Text.literal("Vulkan Frame Generation (AFMF): " + (VPEVulkanClient.enableFrameGen ? "ON" : "OFF")),
                button -> {
                    VPEVulkanClient.enableFrameGen = !VPEVulkanClient.enableFrameGen;
                    this.client.setScreen(new VPEScreen(this.parent));
                }).dimensions(this.width / 2 - 125, startY + 24, 250, 20).build());

        this.addDrawableChild(ButtonWidget.builder(
                Text.literal("Modo Multi-Frame Gen: x" + FSR3NativeBridge.fgMultiplier +
                        (FSR3NativeBridge.fgMultiplier == 2 ? " (Estándar AFMF)" : " (Multi-FG)")),
                button -> {
                    FSR3NativeBridge.fgMultiplier++;
                    if (FSR3NativeBridge.fgMultiplier > 6) {
                        FSR3NativeBridge.fgMultiplier = 2;
                    }
                    this.client.setScreen(new VPEScreen(this.parent));
                }).dimensions(this.width / 2 - 125, startY + 48, 250, 20).build());

        this.addDrawableChild(ButtonWidget.builder(
                Text.literal("Modo FSR 3: " + getFsrPresetName(VPEVulkanClient.fsrRenderScale)),
                button -> {
                    VPEVulkanClient.fsrRenderScale = getNextFsrPresetScale(VPEVulkanClient.fsrRenderScale);
                    this.client.setScreen(new VPEScreen(this.parent));
                }).dimensions(this.width / 2 - 125, startY + 72, 250, 20).build());

        this.addDrawableChild(ButtonWidget.builder(
                Text.literal("Aplicar en Swapchain"),
                button -> {
                    try {
                        long instancePtr = VulkanExtractor.getVkInstanceAddress();
                        long physicalDevicePtr = VulkanExtractor.getVkPhysicalDeviceAddress();
                        long devicePtr = VulkanExtractor.getVkDeviceAddress();
                        long swapchainPtr = VulkanExtractor.getVkSwapchainAddress();
                        int[] resolution = VulkanExtractor.getSwapchainResolution();

                        FSR3NativeBridge.initFSR3(
                                instancePtr, physicalDevicePtr, devicePtr, swapchainPtr, resolution[0], resolution[1]
                        );
                    } catch (Exception e) {
                        e.printStackTrace();
                    }
                    this.client.setScreen(this.parent);
                }).dimensions(this.width / 2 - 125, startY + 104, 250, 20).build());
    }

    @Override
    public void render(DrawContext context, int mouseX, int mouseY, float delta) {
        this.renderBackground(context);
        context.drawCenteredTextWithShadow(this.textRenderer, this.title, this.width / 2, 20, 0xFFFFFF);
        context.drawCenteredTextWithShadow(
                this.textRenderer,
                Text.literal("Render Backend: Vulkan | AFMF Multi-Frame Gen (x" + FSR3NativeBridge.fgMultiplier + ") + RCAS"),
                this.width / 2, 40, 0x00FF00
        );
        super.render(context, mouseX, mouseY, delta);
    }
}