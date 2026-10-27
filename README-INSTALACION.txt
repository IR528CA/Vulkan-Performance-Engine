=== VULKAN PERFORMANCE ENGINE (VPE) v2.0 ===
Minecraft: 26.3
Backend Requerido: Vulkan 1.2+ (Activar en Opciones de Video -> Graphics API)

ARQUITECTURA DE RENDIMIENTO:
Gracias a la transición nativa a Vulkan en Minecraft 26.3, VPE ya no utiliza resoluciones forzadas en OpenGL. 
Este mod intercepta directamente el SwapchainManager para reducir el tamaño de los Attachments de color, 
y utiliza un Mixin en VulkanRenderer.submitCommandBuffers para despachar los algoritmos espaciales 
y temporales directamente a la GPU a través de colas de computación de alta prioridad.

COMPATIBILIDAD MACOS:
Se han verificado los enlaces a través de MoltenVK. Frame Generation puede presentar limitaciones 
dependiendo de las capacidades de sincronización asíncrona (Async Compute) en hardware Apple Silicon.
