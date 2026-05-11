# The list of OpenXR functions this layer overrides.
#
# Adding a function here regenerates dispatch.gen.{h,cpp} on the next
# build (PreBuildEvent in the .vcxproj). You then override the matching
# virtual method on OpenXrLayer (layer.h / layer.cpp) and the framework
# calls it instead of forwarding to the runtime.
#
# IMPORTANT: a small set of functions are already wired up by the
# framework and MUST NOT appear in this list. Adding them here makes
# dispatch_generator.py raise an exception that aborts the build:
#
#   xrCreateInstance
#   xrDestroySession
#   xrDestroyInstance
#   xrGetInstanceProcAddr
#   xrEnumerateInstanceExtensionProperties
#
# You override those by just declaring the virtual method on
# OpenXrLayer; the framework already routes them. See layer.cpp for
# the xrCreateInstance override shipped with this template.
#
# Common candidates to add here, uncomment as you need them:
#   "xrGetSystem",                        # detect target HMD / runtime
#   "xrCreateSession",                    # spin up per-session state
#   "xrEnumerateViewConfigurationViews",  # resize swapchain (FOV crop, super-res, etc.)
#   "xrLocateViews",                      # rewrite per-eye pose / FOV
#   "xrEndFrame",                         # inspect or extend submitted layers
override_functions = [
]

# Extra OpenXR functions the layer wants to *call* on the runtime (in
# addition to anything in override_functions, which is also callable
# downstream automatically).
#
# Add only what you actually use — every entry forces the loader to
# resolve the function pointer at layer init, and a runtime that does
# not implement an unused entry would log a noisy "function not
# present" message for no reason.
requested_functions = [
    "xrGetInstanceProperties",
    "xrGetSystemProperties",
    # Swapchain plumbing (uncomment if you create your own swapchains,
    # e.g. for an overlay quad):
    # "xrCreateReferenceSpace",
    # "xrDestroySpace",
    # "xrEnumerateSwapchainFormats",
    # "xrCreateSwapchain",
    # "xrDestroySwapchain",
    # "xrEnumerateSwapchainImages",
    # "xrAcquireSwapchainImage",
    # "xrWaitSwapchainImage",
    # "xrReleaseSwapchainImage",
]

# OpenXR extensions this layer either provides itself OR consumes from
# the runtime (in which case the loader's negotiation step is what
# matters). Empty for a pass-through skeleton.
extensions = []
