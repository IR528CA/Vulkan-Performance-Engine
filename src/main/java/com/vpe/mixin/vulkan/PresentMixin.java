package com.vpe.mixin.vulkan;

import net.vulkanmod.vulkan.Renderer;
import org.spongepowered.asm.mixin.Mixin;

@Mixin(value = Renderer.class, remap = false)
public class PresentMixin {
    // Reemplazado por RendererMixin para evitar doble inyección en submitFrame.
}