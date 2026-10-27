package com.vpe.core;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.keybinding.v1.KeyBindingHelper;
import net.minecraft.client.option.KeyBinding;
import net.minecraft.client.util.InputUtil;
import org.lwjgl.glfw.GLFW;
import com.vpe.gui.VPEScreen;

public class VPEVulkanClient implements ClientModInitializer {
    public static float fsrRenderScale = 0.77f; // Ultra Quality preset base
    public static boolean enableFSR = true;
    public static boolean enableFrameGen = false;
    private static KeyBinding menuKey;

    @Override
    public void onInitializeClient() {
        System.out.println("[VPE] Acoplando FSR al motor Vulkan...");
        menuKey = KeyBindingHelper.registerKeyBinding(new KeyBinding(
                "key.vpe.open_menu", InputUtil.Type.KEYSYM, GLFW.GLFW_KEY_V, "category.vpe.general"
        ));

        ClientTickEvents.END_CLIENT_TICK.register(client -> {
            while (menuKey.wasPressed()) {
                client.setScreen(new VPEScreen(client.currentScreen));
            }
        });
    }
}