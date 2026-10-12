// SPDX-License-Identifier: GPL-3.0-only
// vkpipe: time how long the Vulkan driver takes to compile a compute SPIR-V (dxc -spirv output). usage: vkpipe k.spv
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);uint32_t*code=malloc(n);fread(code,1,n,f);fclose(f);
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&ai;VkInstance inst;
  if(vkCreateInstance(&ici,0,&inst))return 1;
  uint32_t pc=1;VkPhysicalDevice pd;vkEnumeratePhysicalDevices(inst,&pc,&pd);
  float pr=1;VkDeviceQueueCreateInfo q={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueCount=1;q.pQueuePriorities=&pr;
  VkPhysicalDeviceVulkan12Features f12={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.shaderInt8=1;f12.shaderFloat16=1;
  VkPhysicalDeviceFeatures2 f2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&f12;f2.features.shaderInt64=1;f2.features.shaderInt16=1;
  VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=&f2;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&q;VkDevice dev;
  if(vkCreateDevice(pd,&dci,0,&dev))return 2;
  VkDescriptorSetLayoutBinding b[32];for(int i=0;i<32;i++){b[i]=(VkDescriptorSetLayoutBinding){i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0};}
  VkDescriptorSetLayoutCreateInfo dl={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=32;dl.pBindings=b;VkDescriptorSetLayout sl;vkCreateDescriptorSetLayout(dev,&dl,0,&sl);
  VkPipelineLayoutCreateInfo pl={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&sl;VkPipelineLayout lay;vkCreatePipelineLayout(dev,&pl,0,&lay);
  VkShaderModuleCreateInfo smi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=n;smi.pCode=code;VkShaderModule sm;if(vkCreateShaderModule(dev,&smi,0,&sm))return 3;
  VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=lay;ci.stage=(VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_COMPUTE_BIT,sm,"main",0};
  double t=now();VkPipeline p;VkResult r=vkCreateComputePipelines(dev,0,1,&ci,0,&p);
  printf("create pipeline: %d in %.2fs\n",r,now()-t);return 0;}
