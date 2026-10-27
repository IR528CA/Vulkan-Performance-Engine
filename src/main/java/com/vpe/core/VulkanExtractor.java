package com.vpe.core;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

public class VulkanExtractor {

    public static long getVkInstanceAddress() {
        try {
            Class<?> vulkanClass = Class.forName("net.vulkanmod.vulkan.Vulkan");
            Field instanceField = vulkanClass.getDeclaredField("instance");
            instanceField.setAccessible(true);
            Object vkInstance = instanceField.get(null);

            Method addressMethod = vkInstance.getClass().getMethod("address");
            return (long) addressMethod.invoke(vkInstance);
        } catch (Exception e) {
            System.err.println("[VPE-Hack] Error robando VkInstance: " + e.getMessage());
            return 0L;
        }
    }

    public static long getVkPhysicalDeviceAddress() {
        try {
            Class<?> deviceManagerClass = Class.forName("net.vulkanmod.vulkan.device.DeviceManager");
            Field physicalDeviceField = deviceManagerClass.getDeclaredField("physicalDevice");
            physicalDeviceField.setAccessible(true);
            Object vkPhysicalDevice = physicalDeviceField.get(null);

            Method addressMethod = vkPhysicalDevice.getClass().getMethod("address");
            return (long) addressMethod.invoke(vkPhysicalDevice);
        } catch (Exception e) {
            System.err.println("[VPE-Hack] Error robando VkPhysicalDevice: " + e.getMessage());
            return 0L;
        }
    }

    public static long getVkDeviceAddress() {
        try {
            Class<?> deviceManagerClass = Class.forName("net.vulkanmod.vulkan.device.DeviceManager");
            Field deviceField = deviceManagerClass.getDeclaredField("vkDevice");
            deviceField.setAccessible(true);
            Object vkDevice = deviceField.get(null);

            Method addressMethod = vkDevice.getClass().getMethod("address");
            return (long) addressMethod.invoke(vkDevice);
        } catch (Exception e) {
            System.err.println("[VPE-Hack] Error robando VkDevice: " + e.getMessage());
            return 0L;
        }
    }

    public static long getVkSwapchainAddress() {
        try {
            Class<?> rendererClass = Class.forName("net.vulkanmod.vulkan.Renderer");
            Method getInstanceMethod = rendererClass.getMethod("getInstance");
            Object rendererInstance = getInstanceMethod.invoke(null);

            Method getSwapChainMethod = rendererClass.getMethod("getSwapChain");
            Object swapChainInstance = getSwapChainMethod.invoke(rendererInstance);

            Class<?> swapChainClass = Class.forName("net.vulkanmod.vulkan.framebuffer.SwapChain");
            Method getIdMethod = swapChainClass.getMethod("getId");

            return (long) getIdMethod.invoke(swapChainInstance);
        } catch (Exception e) {
            System.err.println("[VPE-Hack] Error robando VkSwapchain: " + e.getMessage());
            return 0L;
        }
    }

    public static int[] getSwapchainResolution() {
        try {
            Class<?> rendererClass = Class.forName("net.vulkanmod.vulkan.Renderer");
            Object rendererInstance = rendererClass.getMethod("getInstance").invoke(null);
            Object swapChainInstance = rendererClass.getMethod("getSwapChain").invoke(rendererInstance);

            Class<?> swapChainClass = Class.forName("net.vulkanmod.vulkan.framebuffer.SwapChain");
            Method getWidthMethod = swapChainClass.getMethod("getWidth");
            Method getHeightMethod = swapChainClass.getMethod("getHeight");

            int width = (int) getWidthMethod.invoke(swapChainInstance);
            int height = (int) getHeightMethod.invoke(swapChainInstance);

            return new int[]{width, height};
        } catch (Exception e) {
            System.err.println("[VPE-Hack] Error robando Resolucion: " + e.getMessage());
            return new int[]{0, 0};
        }
    }
}