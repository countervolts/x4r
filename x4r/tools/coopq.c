// SPDX-License-Identifier: GPL-3.0-only
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
static const char* ct(int t){switch(t){case 0:return "f16";case 1:return "f32";case 2:return "f64";case 3:return "s8";case 4:return "s16";case 5:return "s32";case 6:return "s64";case 7:return "u8";case 8:return "u16";case 9:return "u32";case 10:return "u64";case 1000141000:return "bf16";case 1000491000:return "s8pk";case 1000491001:return "u8pk";}return "?";}
int main(){VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.apiVersion=VK_API_VERSION_1_3;VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&ai;VkInstance inst;vkCreateInstance(&ici,0,&inst);
 uint32_t pc=1;VkPhysicalDevice pd;vkEnumeratePhysicalDevices(inst,&pc,&pd);
 PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR f=(PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)vkGetInstanceProcAddr(inst,"vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
 uint32_t n=0;f(pd,&n,0);VkCooperativeMatrixPropertiesKHR*p=calloc(n,sizeof*p);for(uint32_t i=0;i<n;i++)p[i].sType=VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;f(pd,&n,p);
 for(uint32_t i=0;i<n;i++)printf("M%u N%u K%u A=%s B=%s C=%s R=%s sat=%d scope=%d\n",p[i].MSize,p[i].NSize,p[i].KSize,ct(p[i].AType),ct(p[i].BType),ct(p[i].CType),ct(p[i].ResultType),p[i].saturatingAccumulation,p[i].scope);}
