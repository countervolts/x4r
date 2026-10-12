// SPDX-License-Identifier: GPL-3.0-only
// vkrun: run a compute SPIR-V (dxc -spirv, -fvk-t-shift 0 -fvk-u-shift 16 -fvk-b-shift 32 -fvk-s-shift 52) on pseudo-random
// resources and dump the storage buffers. usage: vkrun k.spv gx gy gz out.bin [seed] [layout-file]
// layout-file lines: "<binding> sbuf|ubo|tex|rwtex|sampler" (as written by x4rwave); default: 0..31 storage, 32..46 uniform, 47 debug.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NB 56
#define BUFSZ (4u<<20)
#define IMW 128
#define IMH 128
static uint32_t rs;
static uint32_t rnd(void){rs^=rs<<13;rs^=rs>>17;rs^=rs<<5;return rs;}
enum {T_SBUF,T_UBO,T_TEX,T_RWTEX,T_SAMP,T_DBG,T_NONE};
static VkDevice dev;static VkPhysicalDeviceMemoryProperties mp;
static uint32_t memtype(uint32_t bits,VkMemoryPropertyFlags w){for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((bits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&w)==w)return i;return 0;}
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb");if(!f){fprintf(stderr,"cannot open %s\n",argv[1]);return 1;}fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);uint32_t*code=malloc(n);fread(code,1,n,f);fclose(f);
  int gx=atoi(argv[2]),gy=atoi(argv[3]),gz=atoi(argv[4]);rs=argc>6?atoi(argv[6]):12345;
  int type[NB];for(int i=0;i<NB;i++)type[i]=i<32?T_SBUF:i<47?T_UBO:i==47?T_DBG:T_NONE;
  if(argc>7){FILE*lf=fopen(argv[7],"r");char t[32];int b;while(lf&&fscanf(lf,"%d %31s",&b,t)==2){type[b]=!strcmp(t,"sbuf")?T_SBUF:!strcmp(t,"ubo")?T_UBO:!strcmp(t,"tex")?T_TEX:!strcmp(t,"rwtex")?T_RWTEX:T_SAMP;}if(lf)fclose(lf);}
  VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&ai;VkInstance inst;if(vkCreateInstance(&ici,0,&inst))return 1;
  uint32_t pc=1;VkPhysicalDevice pd;vkEnumeratePhysicalDevices(inst,&pc,&pd);
  float pr=1;VkDeviceQueueCreateInfo q={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueCount=1;q.pQueuePriorities=&pr;
  VkPhysicalDeviceVulkan12Features f12={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.shaderInt8=1;f12.shaderFloat16=1;
  VkPhysicalDeviceVulkan13Features f13={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};f13.shaderIntegerDotProduct=1;f12.pNext=&f13;
  VkPhysicalDeviceFeatures2 f2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&f12;f2.features.shaderInt64=1;f2.features.shaderInt16=1;f2.features.shaderStorageImageReadWithoutFormat=1;f2.features.shaderStorageImageWriteWithoutFormat=1;
  VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=&f2;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&q;if(vkCreateDevice(pd,&dci,0,&dev))return 2;
  VkQueue queue;vkGetDeviceQueue(dev,0,0,&queue);vkGetPhysicalDeviceMemoryProperties(pd,&mp);
  VkMemoryPropertyFlags HV=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkBuffer buf[NB]={0};VkDeviceMemory mem[NB];uint32_t*map[NB]={0};
  for(int i=0;i<NB;i++){
    if(type[i]!=T_SBUF&&type[i]!=T_UBO&&type[i]!=T_DBG)continue;
    VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=type[i]==T_UBO?65536:(type[i]==T_DBG?(128u<<20):BUFSZ);bi.usage=type[i]==T_UBO?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vkCreateBuffer(dev,&bi,0,&buf[i]);VkMemoryRequirements mr;vkGetBufferMemoryRequirements(dev,buf[i],&mr);
    VkMemoryAllocateInfo ma={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memtype(mr.memoryTypeBits,HV);vkAllocateMemory(dev,&ma,0,&mem[i]);vkBindBufferMemory(dev,buf[i],mem[i],0);
    vkMapMemory(dev,mem[i],0,bi.size,0,(void**)&map[i]);
    if(type[i]==T_DBG){memset(map[i],0,bi.size);continue;}
    for(uint32_t k=0;k<bi.size/4;k++){uint32_t r=rnd();
      if(type[i]==T_UBO) map[i][k]=4+(r%12);
      else if(i>=16) map[i][k]=0;
      else { float v=((int32_t)(r&0xffff)-32768)/16384.0f; memcpy(&map[i][k],&v,4); if(rnd()&1) map[i][k]=r; } }
  }
  // images: 128x128 RGBA32F, sampled ones filled with floats in [0,1) (same stream as the oracle)
  VkImage img[NB]={0};VkImageView iv[NB]={0};VkDeviceMemory imem[NB];
  VkBuffer stg=0;VkDeviceMemory stgm;float*stgp=0;
  {VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=IMW*IMH*16;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;vkCreateBuffer(dev,&bi,0,&stg);VkMemoryRequirements mr;vkGetBufferMemoryRequirements(dev,stg,&mr);
   VkMemoryAllocateInfo ma={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memtype(mr.memoryTypeBits,HV);vkAllocateMemory(dev,&ma,0,&stgm);vkBindBufferMemory(dev,stg,stgm,0);vkMapMemory(dev,stgm,0,bi.size,0,(void**)&stgp);}
  VkCommandPoolCreateInfo cpi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};VkCommandPool cp;vkCreateCommandPool(dev,&cpi,0,&cp);
  VkCommandBufferAllocateInfo cai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=cp;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VkCommandBuffer cb;vkAllocateCommandBuffers(dev,&cai,&cb);
  for(int i=0;i<NB;i++){
    if(type[i]!=T_TEX&&type[i]!=T_RWTEX)continue;
    for(int k=0;k<IMW*IMH*4;k++)stgp[k]=type[i]==T_TEX?(rnd()&0xffff)/65536.0f:0.0f;
    VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R32G32B32A32_SFLOAT;ii.extent=(VkExtent3D){IMW,IMH,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(dev,&ii,0,&img[i]);VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,img[i],&mr);
    VkMemoryAllocateInfo ma={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memtype(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);vkAllocateMemory(dev,&ma,0,&imem[i]);vkBindImageMemory(dev,img[i],imem[i],0);
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=img[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;vi.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCreateImageView(dev,&vi,0,&iv[i]);
    VkCommandBufferBeginInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};cbi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vkBeginCommandBuffer(cb,&cbi);
    VkImageMemoryBarrier b1={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b1.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b1.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b1.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b1.srcQueueFamilyIndex=b1.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b1.image=img[i];b1.subresourceRange=vi.subresourceRange;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&b1);
    VkBufferImageCopy bc={0,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},{IMW,IMH,1}};vkCmdCopyBufferToImage(cb,stg,img[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bc);
    VkImageMemoryBarrier b2=b1;b2.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b2.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;b2.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b2.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,0,0,0,1,&b2);
    vkEndCommandBuffer(cb);VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;vkQueueSubmit(queue,1,&si,0);vkQueueWaitIdle(queue);vkResetCommandBuffer(cb,0);
  }
  VkSampler smp;{VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;vkCreateSampler(dev,&si,0,&smp);}
  VkDescriptorType dt[NB];VkDescriptorSetLayoutBinding b[NB];
  for(int i=0;i<NB;i++){dt[i]=type[i]==T_UBO?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:type[i]==T_TEX?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:type[i]==T_RWTEX?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:type[i]==T_SAMP?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[i]=(VkDescriptorSetLayoutBinding){i,dt[i],1,VK_SHADER_STAGE_COMPUTE_BIT,0};}
  VkDescriptorSetLayoutCreateInfo dl={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=NB;dl.pBindings=b;VkDescriptorSetLayout sl;vkCreateDescriptorSetLayout(dev,&dl,0,&sl);
  VkPipelineLayoutCreateInfo pl={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&sl;VkPipelineLayout lay;vkCreatePipelineLayout(dev,&pl,0,&lay);
  VkShaderModuleCreateInfo smi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=n;smi.pCode=code;VkShaderModule sm;if(vkCreateShaderModule(dev,&smi,0,&sm))return 3;
  VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=lay;ci.stage=(VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,0,0,VK_SHADER_STAGE_COMPUTE_BIT,sm,"main",0};
  VkPipeline p;if(vkCreateComputePipelines(dev,0,1,&ci,0,&p))return 4;
  VkDescriptorPoolSize ps[5]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,64},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,32},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,32},{VK_DESCRIPTOR_TYPE_SAMPLER,4}};
  VkDescriptorPoolCreateInfo dpi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=1;dpi.poolSizeCount=5;dpi.pPoolSizes=ps;VkDescriptorPool dp;vkCreateDescriptorPool(dev,&dpi,0,&dp);
  VkDescriptorSetAllocateInfo dai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=dp;dai.descriptorSetCount=1;dai.pSetLayouts=&sl;VkDescriptorSet ds;vkAllocateDescriptorSets(dev,&dai,&ds);
  VkWriteDescriptorSet w[NB];VkDescriptorBufferInfo bi2[NB];VkDescriptorImageInfo ii2[NB];
  for(int i=0;i<NB;i++){
    w[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,0,ds,i,0,1,dt[i],0,0,0};
    if(type[i]==T_TEX||type[i]==T_RWTEX){ii2[i]=(VkDescriptorImageInfo){0,iv[i],VK_IMAGE_LAYOUT_GENERAL};w[i].pImageInfo=&ii2[i];}
    else if(type[i]==T_SAMP||type[i]==T_NONE){ii2[i]=(VkDescriptorImageInfo){smp,0,VK_IMAGE_LAYOUT_UNDEFINED};w[i].pImageInfo=&ii2[i];w[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;}
    else {bi2[i]=(VkDescriptorBufferInfo){buf[i],0,VK_WHOLE_SIZE};w[i].pBufferInfo=&bi2[i];}
  }
  vkUpdateDescriptorSets(dev,NB,w,0,0);
  int timeit=getenv("X4R_TIME")!=0;int reps=timeit?atoi(getenv("X4R_TIME")):1;if(reps<1)reps=1;
  VkQueryPool qp=0;if(timeit){VkQueryPoolCreateInfo qi={VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2*reps;vkCreateQueryPool(dev,&qi,0,&qp);}
  VkCommandBufferBeginInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkBeginCommandBuffer(cb,&cbi);
  if(timeit)vkCmdResetQueryPool(cb,qp,0,2*reps);
  vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,p);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,lay,0,1,&ds,0,0);
  for(int rp=0;rp<reps;rp++){
    if(timeit){VkMemoryBarrier mb={VK_STRUCTURE_TYPE_MEMORY_BARRIER,0,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,0,0,0);vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,2*rp);}
    vkCmdDispatch(cb,gx,gy,gz);
    if(timeit)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,2*rp+1);}
  vkEndCommandBuffer(cb);
  VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;vkQueueSubmit(queue,1,&si,0);
  VkResult r=vkQueueWaitIdle(queue);if(r){fprintf(stderr,"queue wait: %d\n",r);return 5;}
  if(timeit){uint64_t ts[256];vkGetQueryPoolResults(dev,qp,0,2*reps,sizeof ts,ts,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(pd,&pp);double best=1e30,sum=0;for(int rp=0;rp<reps;rp++){double ms=(ts[2*rp+1]-ts[2*rp])*pp.limits.timestampPeriod*1e-6;if(ms<best)best=ms;sum+=ms;}printf("GPU time: best %.3f ms avg %.3f ms over %d runs\n",best,sum/reps,reps);}
  FILE*o=fopen(argv[5],"wb");for(int i=16;i<31;i++)if(map[i]&&type[i]==T_SBUF)fwrite(map[i],1,BUFSZ,o);fclose(o);
  {char nm[512];snprintf(nm,sizeof nm,"%s.dbg",argv[5]);FILE*d=fopen(nm,"wb");fwrite(map[47],1,128u<<20,d);fclose(d);}return 0;}
