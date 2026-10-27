package com.vpe.mixin.vulkan;

import net.minecraft.client.util.Window;
import org.spongepowered.asm.mixin.Mixin;

@Mixin(Window.class)
public class WindowMixin {
    // Mantenemos Window 100% en la resolución nativa del monitor.
    // Así el puntero (+), el chat y la barra de inventario nunca hacen zoom
    // ni cambian de tamaño al abrir o cerrar el menú de pausa o el inventario.
}