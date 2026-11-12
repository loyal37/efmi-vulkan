// Included after m2_gpu_case.h. A three-component character drawn through the loaded DLL hooks:
// one component replaced on its own bones, one whose replacement also reads the first
// component's bones (the merged skeleton), one the mod hides, and a second LOD with a bone remap.
// The package is written by sandbox/character_test.py; positions below must agree with it.

static void run_character(Gpu& g,bool use_rp2,bool material=false,bool reject_late=false,bool lazy_packages=false)
{
    auto recipe=m2_fixture::recipe();
    RecipeFixture layout_fixture(material);auto layout=layout_fixture.layout;
    // D-133: the application's fragment shader with the character dither, for the scene that runs RabbitFX's
    // anti-dither (EFMI_VK_TEST_DITHER=1 in this process's environment): its pictures must be the same.
    const bool dither=[]{char value[4]={};return GetEnvironmentVariableA("EFMI_VK_TEST_DITHER",value,sizeof(value)) && value[0]=='1';}();
    // D-176: with EFMI_VK_TEST_OWN_MESH=1 a section of the scene's Mods folder hides the draw no section of
    // the character matches and draws a triangle of its own, moved by (0,.85), under the application's
    // vertex stage: on that draw's own bone it stands at (.6,.75) with the application's shading.
    const bool own_mesh=[]{char value[4]={};return GetEnvironmentVariableA("EFMI_VK_TEST_OWN_MESH",value,sizeof(value)) && value[0]=='1';}();
    // D-178: with EFMI_VK_TEST_UNMATCHED_SHADE=N a section of the scene's Mods folder gives the draw no section of
    // the character matches a texture of its own (the application's draw again with it, D-083): that draw shows
    // shade N once its package is on the device.
    const int unmatched_shade=[]{char value[8]={};return GetEnvironmentVariableA("EFMI_VK_TEST_UNMATCHED_SHADE",value,sizeof(value)) ? atoi(value) : 1;}();
    if(material){
        auto fs=std::make_shared<ShaderInfo>();fs->id=2;
        fs->code=variant_fixture::shader(false,true,6,dither?variant_fixture::Kill::Dither:variant_fixture::Kill::None);
        fs->size=static_cast<uint32_t>(fs->code.size()*4);
        recipe.stages[1].shader=fs;recipe.blend_attachments[0].blendEnable=VK_FALSE;
    }
    if(use_rp2){auto pass=std::make_shared<RenderPassRecipe>(*recipe.render_pass);pass->version2=true;recipe.render_pass=pass;}
    VK(recipe.render_pass->create(g.device,vkCreateRenderPass,vkCreateRenderPass2,&g.pass));
    for(const auto& set:layout.sets){
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for(const auto& p:set->bindings)bindings.push_back({p.first,static_cast<VkDescriptorType>(p.second.type),p.second.count,p.second.stages,nullptr});
        VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount=static_cast<uint32_t>(bindings.size());ci.pBindings=bindings.data();
        VkDescriptorSetLayout s;VK(vkCreateDescriptorSetLayout(g.device,&ci,nullptr,&s));g.sets.push_back(s);
    }
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=3;li.pSetLayouts=g.sets.data();
    VK(vkCreatePipelineLayout(g.device,&li,nullptr,&g.layout));
    std::vector<VkShaderModule> modules;
    for(const auto& stage:recipe.stages){
        VkShaderModuleCreateInfo ci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=stage.shader->code.size()*4;ci.pCode=stage.shader->code.data();
        VkShaderModule s;VK(vkCreateShaderModule(g.device,&ci,nullptr,&s));modules.push_back(s);
    }
    VK(recipe.create(g.device,vkCreateGraphicsPipelines,g.layout,g.pass,modules,&g.pipeline));
    // A second pipeline of the application whose fragment shader reads a fourth set. Its layout agrees
    // with the character's for sets 0 to 2, so a set 3 bound for it stays valid across the character's
    // draws, which is the state the game is in when a character draw follows a four-set draw.
    struct Extra{VkDevice device;VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
        ~Extra(){vkDeviceWaitIdle(device);if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(device,layout,nullptr);}
    } four{g.device};
    {
        VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=1;ci.pBindings=&binding;
        VkDescriptorSetLayout s;VK(vkCreateDescriptorSetLayout(g.device,&ci,nullptr,&s));g.sets.push_back(s);
        VkPipelineLayoutCreateInfo li4={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li4.setLayoutCount=4;li4.pSetLayouts=g.sets.data();
        VK(vkCreatePipelineLayout(g.device,&li4,nullptr,&four.layout));
        const auto code=m2_fixture::shader(false,true);
        VkShaderModuleCreateInfo ci4={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci4.codeSize=code.size()*4;ci4.pCode=code.data();
        VkShaderModule fragment;VK(vkCreateShaderModule(g.device,&ci4,nullptr,&fragment));
        size_t fs=0;while(recipe.stages[fs].stage!=VK_SHADER_STAGE_FRAGMENT_BIT)++fs;
        auto with_extra=modules;with_extra[fs]=fragment;
        VK(recipe.create(g.device,vkCreateGraphicsPipelines,four.layout,g.pass,with_extra,&four.pipeline));
        vkDestroyShaderModule(g.device,fragment,nullptr);
    }
    // Material runs: the character's fragment shader once more with its image at binding 8 of set
    // 1 instead of 6. The package has no profile for these bytes. The application binds the same
    // texture there, in a set of the layout this pipeline needs.
    struct Shifted{VkDevice device;VkDescriptorSetLayout set=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
        ~Shifted(){vkDeviceWaitIdle(device);if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
                   if(set)vkDestroyDescriptorSetLayout(device,set,nullptr);}
    } shifted{g.device};
    if(material){
        VkDescriptorSetLayoutBinding binding={8,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=1;ci.pBindings=&binding;
        VK(vkCreateDescriptorSetLayout(g.device,&ci,nullptr,&shifted.set));
        VkDescriptorSetLayout layouts[]={g.sets[0],shifted.set,g.sets[2]};
        VkPipelineLayoutCreateInfo ls={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};ls.setLayoutCount=3;ls.pSetLayouts=layouts;
        VK(vkCreatePipelineLayout(g.device,&ls,nullptr,&shifted.layout));
        const auto code=variant_fixture::shader(false,true,8);
        VkShaderModuleCreateInfo cs={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};cs.codeSize=code.size()*4;cs.pCode=code.data();
        VkShaderModule fragment;VK(vkCreateShaderModule(g.device,&cs,nullptr,&fragment));
        size_t fs=0;while(recipe.stages[fs].stage!=VK_SHADER_STAGE_FRAGMENT_BIT)++fs;
        auto with_shifted=modules;with_shifted[fs]=fragment;
        VK(recipe.create(g.device,vkCreateGraphicsPipelines,shifted.layout,g.pass,with_shifted,&shifted.pipeline));
        vkDestroyShaderModule(g.device,fragment,nullptr);
    }
    for(auto module:modules)vkDestroyShaderModule(g.device,module,nullptr);
    Image color=g.image(VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth=g.image(VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    Image original_image{};
    if(material)original_image=g.image(VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView views[]={color.view,depth.view};
    VkFramebufferCreateInfo fi={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=g.pass;fi.attachmentCount=2;fi.pAttachments=views;fi.width=fi.height=64;fi.layers=1;
    VK(vkCreateFramebuffer(g.device,&fi,nullptr,&g.framebuffer));

    // The game's own mesh: one triangle on bone zero of whichever component's palette the entry names.
    const float positions[12]={-.22f,-.2f,0,0, .22f,-.2f,0,0, 0,.22f,0,0};
    const float uv[6]={};
    struct Blend{uint16_t weight[4];uint8_t index[4];};
    const Blend blends[]={{{65535,0,0,0},{0,0,0,0}},{{65535,0,0,0},{0,0,0,0}},{{65535,0,0,0},{0,0,0,0}}};
    // Index regions: component A (3 at 0), B (6 at 3), C (9 at 9), A at LOD 1 (12 at 18), an unmatched draw (3 at 30).
    // Padding triangles are degenerate, so every region draws one triangle.
    uint16_t indices[33]={};
    for(uint32_t first:{0u,3u,9u,18u,30u}){indices[first]=0;indices[first+1]=1;indices[first+2]=2;}
    auto pos=g.buffer(sizeof(positions),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);memcpy(pos.mapped,positions,sizeof(positions));
    auto tex=g.buffer(sizeof(uv),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);memcpy(tex.mapped,uv,sizeof(uv));
    auto blend=g.buffer(sizeof(blends),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);memcpy(blend.mapped,blends,sizeof(blends));
    auto index=g.buffer(sizeof(indices),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);memcpy(index.mapped,indices,sizeof(indices));
    auto bones=g.buffer(65536,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto row=[&](uint32_t r,float x,float y){float v[4]={x,y,0,0};memcpy(static_cast<uint8_t*>(bones.mapped)+r*16,v,16);};
    float tint[]={material?1.f:0.f,1,1,1};memcpy(bones.mapped,tint,16);
    if(material)memcpy(static_cast<uint8_t*>(bones.mapped)+258*16,tint,16);
    // Palettes: a component's bone k is the matrix at offset + 3 + 3 * k; only its first row is used.
    enum{A_CUR=2000,A_PREV=2100,B_CUR=2200,B_PREV=2300,C_CUR=2400,C_PREV=2500,L_CUR=2600,L_PREV=2700,U_CUR=2800,U_PREV=2900,
         N_CUR=3000,N_PREV=3100,X_CUR=3200,X_PREV=3300,Y_CUR=3400,Y_PREV=3500};
    row(A_CUR+3,-.6f,-.6f);row(A_CUR+6,-.6f,.6f);
    row(B_CUR+3,0,-.6f);row(B_CUR+6,0,0);
    row(C_CUR+3,.6f,-.6f);
    row(L_CUR+3,-.2f,.6f);row(L_CUR+6,-.6f,-.2f);
    row(U_CUR+3,.6f,-.1f);
    row(N_CUR+3,0,.6f);
    // The two four-set draws stand where components A and B stood before they were replaced.
    row(X_CUR+3,-.6f,-.6f);row(Y_CUR+3,0,-.6f);
    const uint32_t dynamic=static_cast<uint32_t>(std::max<VkDeviceSize>(256,g.properties.limits.minUniformBufferOffsetAlignment));
    auto instance=g.buffer(8*dynamic+65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto entry=[&](uint32_t slot,uint32_t current,uint32_t previous,float green,float x=10){
        uint8_t* e=static_cast<uint8_t*>(instance.mapped)+slot*dynamic;
        float shade[]={1,green,material?1.f:0.f,.5f};memcpy(e+16,shade,16);
        float place[]={x,20,30,1};memcpy(e+48,place,16);
        uint32_t offsets[]={current,previous,258,0x3f800000};memcpy(e+80,offsets,16);
    };
    enum{E_A,E_B,E_C,E_L,E_U,E_N,E_X,E_Y};
    entry(E_A,A_CUR,A_PREV,1);entry(E_B,B_CUR,B_PREV,.5f);entry(E_C,C_CUR,C_PREV,1);entry(E_L,L_CUR,L_PREV,1);entry(E_U,U_CUR,U_PREV,1);
    // Another object, a quarter of a unit away, that happens to use component B's mesh.
    entry(E_N,N_CUR,N_PREV,1,10.25f);
    entry(E_X,X_CUR,X_PREV,1,50);entry(E_Y,Y_CUR,Y_PREV,1,50);
    // The block of the fourth set, read at a nonzero dynamic offset: its color is a quarter green.
    auto extra=g.buffer(dynamic+65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    {float shade[]={1,.25f,0,.5f};memcpy(static_cast<uint8_t*>(extra.mapped)+dynamic+16,shade,16);}
    auto readback=g.buffer(64*64*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,2},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2}};
    VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=5;pi.poolSizeCount=material?3:2;pi.pPoolSizes=sizes;
    VK(vkCreateDescriptorPool(g.device,&pi,nullptr,&g.descriptors));
    VkDescriptorSet shifted_set=VK_NULL_HANDLE;
    VkDescriptorSet sets[4];VkDescriptorSetAllocateInfo si={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    si.descriptorPool=g.descriptors;si.descriptorSetCount=4;si.pSetLayouts=g.sets.data();VK(vkAllocateDescriptorSets(g.device,&si,sets));
    VkDescriptorBufferInfo infos[]={{bones.handle,0,65536},{instance.handle,0,65536},{extra.handle,0,65536}};
    VkWriteDescriptorSet writes[3]={};
    for(uint32_t i=0;i<3;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=sets[i==0?0:i+1];writes[i].dstBinding=i?0:7;
        writes[i].descriptorCount=1;writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo=&infos[i];}
    vkUpdateDescriptorSets(g.device,3,writes,0,nullptr);
    if(material){
        VkDescriptorImageInfo image{VK_NULL_HANDLE,original_image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=sets[1];write.dstBinding=6;
        write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.descriptorCount=1;write.pImageInfo=&image;
        vkUpdateDescriptorSets(g.device,1,&write,0,nullptr);
        VkDescriptorSetAllocateInfo ss={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ss.descriptorPool=g.descriptors;ss.descriptorSetCount=1;ss.pSetLayouts=&shifted.set;VK(vkAllocateDescriptorSets(g.device,&ss,&shifted_set));
        write.dstSet=shifted_set;write.dstBinding=8;
        vkUpdateDescriptorSets(g.device,1,&write,0,nullptr);
    }
    VkCommandPoolCreateInfo pc={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=g.family;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(vkCreateCommandPool(g.device,&pc,nullptr,&g.pool));
    VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=g.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
    VkCommandBuffer cmd;VK(vkAllocateCommandBuffers(g.device,&ca,&cmd));

    // 0 background, 1 a draw shaded by component A's (or C's, or the unmatched draw's) entry, 2 by B's,
    // 4 by the block of the fourth set, 3 anything else.
    auto shade_at=[&](float x,float y){
        const uint32_t px=static_cast<uint32_t>((x+1)*32),py=static_cast<uint32_t>((y+1)*32);
        if(px>=64 || py>=64)return 3;
        const uint8_t* p=static_cast<uint8_t*>(readback.mapped)+4*(py*64+px);
        if(material){
            if(p[0]==0 && p[1]==0 && p[2]==255 && p[3]==255)return 0;
            if(p[0]==255 && p[1]==255 && p[2]==255 && p[3]>=126 && p[3]<=129)return 1;
            if(p[0]==255 && p[1]>=126 && p[1]<=129 && p[2]==255)return 2;
            if(p[0]==255 && p[1]>=62 && p[1]<=66 && p[2]==0)return 4;
            if(p[0]==255 && p[1]==0 && p[2]==0 && p[3]>=126 && p[3]<=129)return 5;
            if(p[0]==0 && p[1]>=126 && p[1]<=129 && p[2]==0 && p[3]>=126 && p[3]<=129)return 6;
            if(p[0]==0 && p[1]==255 && p[2]==0 && p[3]>=126 && p[3]<=129)return 8;
            if(p[0]==0 && p[1]==0 && p[2]==0 && p[3]==0)return 7;
            return 3;
        }
        if(p[0]==0 && p[1]==0 && p[2]==255)return 0;
        if(p[0]==0 && p[1]>=126 && p[1]<=129 && p[2]>=126 && p[2]<=129)return 1;
        if(p[0]==0 && p[1]>=62 && p[1]<=66 && p[2]>=126 && p[2]<=129)return 2;
        if(p[0]==0 && p[1]>=30 && p[1]<=34 && p[2]>=126 && p[2]<=129)return 4;
        return 3;
    };
    bool image_initialized=false;
    auto frame=[&](bool lod1,bool four_sets=false,bool character_on_four=false,bool character_on_shifted=false,bool presented=true){
        VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK(vkBeginCommandBuffer(cmd,&begin));
        if(material && !image_initialized){
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=original_image.handle;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkClearColorValue white{{1,1,1,1}};vkCmdClearColorImage(cmd,b.image,b.newLayout,&white,1,&b.subresourceRange);
            b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=b.dstAccessMask;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
            image_initialized=true;
        }
        VkClearValue clear[2]={};clear[0].color.float32[2]=clear[0].color.float32[3]=1;clear[1].depthStencil.depth=1;
        VkRenderPassBeginInfo bi={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};bi.renderPass=g.pass;bi.framebuffer=g.framebuffer;
        bi.renderArea.extent={64,64};bi.clearValueCount=2;bi.pClearValues=clear;vkCmdBeginRenderPass(cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp={0,0,64,64,0,1};VkRect2D sc={{0,0},{64,64}};vkCmdSetViewport(cmd,0,1,&vp);vkCmdSetScissor(cmd,0,1,&sc);
        VkBuffer streams[]={pos.handle,tex.handle,blend.handle,pos.handle};VkDeviceSize offsets[]={0,0,0,0};
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline);vkCmdBindVertexBuffers(cmd,0,4,streams,offsets);
        vkCmdBindIndexBuffer(cmd,index.handle,0,VK_INDEX_TYPE_UINT16);
        auto bind=[&](uint32_t slot){uint32_t offset=slot*dynamic;
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.layout,0,3,sets,1,&offset);};
        if(four_sets){
            // The application draws with its four-set pipeline, then switches to the character's
            // pipeline and binds only sets 0 to 2: its set 3 stays bound and valid.
            uint32_t offsets[]={E_X*dynamic,dynamic};
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,four.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,four.layout,0,4,sets,2,offsets);
            vkCmdDrawIndexed(cmd,3,1,30,0,0);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline);
        }
        if(lod1){bind(E_L);vkCmdDrawIndexed(cmd,12,1,18,0,0);}
        else if(character_on_four){
            // Component A's own region drawn with the application's four-set pipeline, whose fragment
            // shader reads set 3: the game does this in the pass that tests which parts are visible.
            uint32_t offsets[]={E_A*dynamic,dynamic};
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,four.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,four.layout,0,4,sets,2,offsets);
            vkCmdDrawIndexed(cmd,3,1,0,0,0);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline);
        }
        else if(character_on_shifted){
            // Component A's own region drawn with the fragment shader the package has no profile for.
            VkDescriptorSet with_shifted[]={sets[0],shifted_set,sets[2]};uint32_t offset=E_A*dynamic;
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shifted.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shifted.layout,0,3,with_shifted,1,&offset);
            vkCmdDrawIndexed(cmd,3,1,0,0,0);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.pipeline);
        }
        else{bind(E_A);vkCmdDrawIndexed(cmd,3,1,0,0,0);}
        bind(E_B);vkCmdDrawIndexed(cmd,6,1,3,0,0);
        bind(E_C);vkCmdDrawIndexed(cmd,9,1,9,0,0);
        // Not a section of the package; drawn right after replaced draws without rebinding pipeline or buffers.
        bind(E_U);vkCmdDrawIndexed(cmd,3,1,30,0,0);
        // The neighbour draws B's index region with its own entry. It is never the character.
        bind(E_N);vkCmdDrawIndexed(cmd,6,1,3,0,0);
        if(four_sets){
            // Back to the four-set pipeline without binding set 3 again: it must be the application's.
            uint32_t offset=E_Y*dynamic;
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,four.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,four.layout,2,1,&sets[2],1,&offset);
            vkCmdDrawIndexed(cmd,3,1,30,0,0);
        }
        vkCmdEndRenderPass(cmd);
        VkBufferImageCopy copy={};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={64,64,1};
        vkCmdCopyImageToBuffer(cmd,color.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
        VkBufferMemoryBarrier barrier={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer=readback.handle;barrier.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
        VK(vkEndCommandBuffer(cmd));
        VK(g.submit(cmd));VK(vkQueueWaitIdle(g.queue));
        // There is no swapchain here: tell the backend that a frame with draws ended.
        if(presented)self_probe("{\"cmd\":\"m2\",\"advance\":1}","character-advance.json");
    };
    // The draw no section of the character matches: the application's triangle at (.6,-.1), or, in D-176's
    // scene, the Mod's own in its place at (.6,.75) once its package is on the device (`live`).
    auto unmatched=[&](bool live=true){return own_mesh ? shade_at(.6f,-.1f)==0 && shade_at(.6f,.75f)==(live?1:0) :
                                                  shade_at(.6f,-.1f)==(live || unmatched_shade==1 ? unmatched_shade : 0);};
    // What the game draws by itself: every component at bone zero of its own palette.
    auto original=[&]{return shade_at(-.6f,-.6f)==1 && shade_at(0,-.6f)==2 && shade_at(.6f,-.6f)==1 && shade_at(.6f,-.1f)==1 && shade_at(0,.6f)==1 &&
                             shade_at(-.6f,.3f)==0 && shade_at(0,0)==0 && shade_at(.6f,.3f)==0;};
    // A new object's first frame, as the Core's identification has it (D-103): its components are drawn
    // neither by the application nor by the mod, and so is the other object sharing the mesh at (0,.6)
    // in its own first frame; the unmatched neighbour at (.6,-.1) stays.
    auto skipped=[&](bool live=true){return shade_at(-.6f,-.6f)==0 && shade_at(0,-.6f)==0 && shade_at(.6f,-.6f)==0 && unmatched(live) && shade_at(0,.6f)==0 &&
                            shade_at(-.6f,.3f)==0 && shade_at(0,0)==0 && shade_at(.6f,.3f)==0;};
    // The shades at the eight places, for a failed check's report.
    auto shades=[&](const char* what){
        fprintf(stderr,"%s: shades %d %d %d | %d %d | %d %d %d\n",what,shade_at(-.6f,-.6f),shade_at(0,-.6f),shade_at(.6f,-.6f),
                shade_at(.6f,-.1f),shade_at(0,.6f),shade_at(-.6f,.3f),shade_at(0,0),shade_at(.6f,.3f));};
    // The package: A's mesh on merged bone 1 moved by (0,-.3); B's on merged bone 3 and, moved by
    // (1.2,.9), on merged bone 0, which is A's; C hidden. `ax` is where A's bone zero currently is.
    const int a_shade=material?5:1,b_shade=material?6:2,cross_shade=material?7:2;
    auto replaced=[&](float ax){return shade_at(-.6f,.3f)==a_shade && shade_at(0,0)==b_shade && shade_at(ax+1.2f,.3f)==cross_shade && shade_at(0,.6f)==1 &&
                                       shade_at(-.6f,-.6f)==0 && shade_at(0,-.6f)==0 && shade_at(.6f,-.6f)==0 && unmatched();};

    // The backend prepares its mesh and arenas on a thread of its own. The game is long past that
    // when a character appears; the scene waits for it, so that no draw meets an empty pool. A
    // package of a directory goes to the GPU on its first match, so there the character's first
    // frame comes before the wait: the application draws it, and the character is known after it.
    // A package not yet on the device: a draw its lists hide and draw again is skipped while it comes up.
    if(lazy_packages){frame(false);if(!skipped(false))shades("first frame of a new character is skipped, as the Core does");check(skipped(false),"first frame of a new character is skipped, as the Core does");}
    bool prepared=false;
    for(uint32_t attempt=0;attempt<1000 && !prepared;++attempt){
        prepared=self_probe("{\"cmd\":\"status\"}","character-ready.json").find("\"ready\":true")!=std::string::npos;
        if(!prepared)Sleep(5);
    }
    check(prepared,"the backend is ready before the scene begins");
    if(!lazy_packages){frame(false);if(!skipped())shades("first frame of a new character is skipped, as the Core does");check(skipped(),"first frame of a new character is skipped, as the Core does");}
    if(reject_late){
        bool refused=false;
        for(uint32_t attempt=0;attempt<400 && !refused;++attempt){
            frame(false);refused=shade_at(-.6f,.3f)==a_shade && shade_at(0,-.6f)==2 && shade_at(0,0)==0 && shade_at(.6f,.3f)==0 &&
                                shade_at(.6f,-.6f)==0 && shade_at(.6f,-.1f)==1;
            if(!refused)Sleep(5);
        }
        check(refused,"late unsupported material preserves the entire original component; other components still replace");
        self_probe("{\"cmd\":\"status\"}","character-final.json");return;
    }
    bool active=false;
    for(uint32_t attempt=0;attempt<400 && !active;++attempt){frame(false);active=replaced(-.6f);if(!active)Sleep(5);}
    check(active,"whole character replaced: own bones, another component's bones, hidden component, restored next draw, neighbour untouched");
    self_probe("{\"cmd\":\"status\"}","character-active.json");
    if(!active)return;
    bool follows=true;
    for(uint32_t i=0;i<20;++i){
        const float ax=i%2?-.5f:-.7f;
        row(A_CUR+3,ax,-.6f);frame(false);follows&=replaced(ax);
    }
    row(A_CUR+3,-.6f,-.6f);
    check(follows,"20 pose changes of one component move the other component's geometry in the same frame");
    if(unmatched_shade!=1){
        // The texture follows the draw: the application's mesh, moved by its own bone, with the Mod's image.
        bool moves=true;
        for(uint32_t i=0;i<6;++i){
            const float ux=i%2?.5f:.6f;
            row(U_CUR+3,ux,-.1f);frame(false);
            moves&=shade_at(ux,-.1f)==unmatched_shade && shade_at(-.6f,.3f)==a_shade && shade_at(0,0)==b_shade;
        }
        row(U_CUR+3,.6f,-.1f);frame(false);
        check(moves && replaced(-.6f),"the application's draw again with a Mod's image, where no character section matches");
        self_probe("{\"cmd\":\"status\"}","character-final.json");
        return;
    }
    if(own_mesh){
        // The Mod's own triangle follows the bone of the draw it stands for, which is the application's.
        bool moves=true;
        for(uint32_t i=0;i<6;++i){
            const float ux=i%2?.5f:.6f;
            row(U_CUR+3,ux,-.1f);frame(false);
            moves&=shade_at(ux,.75f)==1 && shade_at(ux,-.1f)==0 && shade_at(-.6f,.3f)==a_shade && shade_at(0,0)==b_shade;
        }
        row(U_CUR+3,.6f,-.1f);frame(false);
        check(moves && replaced(-.6f),"a Mod's own mesh under the application's vertex stage follows the application's bone of its draw");
        self_probe("{\"cmd\":\"m2\",\"enabled\":0}","character-disabled.json");
        frame(false);
        check(original() && shade_at(.6f,.75f)==0,"runtime disable returns the draw to the application's own mesh");
        self_probe("{\"cmd\":\"m2\",\"enabled\":1}","character-enabled.json");
        bool again=false;
        for(uint32_t attempt=0;attempt<100 && !again;++attempt){frame(false);again=replaced(-.6f);}
        check(again,"runtime enable draws the Mod's own mesh again");
        self_probe("{\"cmd\":\"status\"}","character-final.json");
        return;
    }
    // The game records its next frame before the previous one is presented: two recordings and no
    // present between them, the character one cell further in the second. It is the same object.
    {
        auto place=[&](float x){for(uint32_t slot:{uint32_t(E_A),uint32_t(E_B),uint32_t(E_C),uint32_t(E_L)}){
            float p[]={x,20,30,1};memcpy(static_cast<uint8_t*>(instance.mapped)+slot*dynamic+48,p,16);}};
        frame(false,false,false,false,false);
        place(9.875f);
        frame(false);
        check(replaced(-.6f),"a frame recorded before the previous one was presented, with the character a cell further, is replaced");
        place(10);
        frame(false);
        check(replaced(-.6f),"and so is the frame after it");
    }
    // Frames end without the character (a dash hides it; with frame generation presents pass between
    // two of its frames). Drawn again shortly after, it is the mod's at once.
    for(uint32_t i=0;i<6;++i)self_probe("{\"cmd\":\"m2\",\"advance\":1}","character-advance.json");
    frame(false);
    check(replaced(-.6f),"six frames without the character do not make it a stranger: replaced in its first frame back");
    // After several frames and longer than the identity expiry without a draw (the test's environment
    // sets half a second; 10 s in the game) it is forgotten: its first frame is skipped, then it is
    // identified and replaced again.
    Sleep(700);
    for(uint32_t i=0;i<6;++i)self_probe("{\"cmd\":\"m2\",\"advance\":1}","character-advance.json");
    frame(false);
    if(!skipped())shades("long enough without a draw, the character is a new object and its first frame is skipped");check(skipped(),"long enough without a draw, the character is a new object and its first frame is skipped");
    bool known=false;
    for(uint32_t attempt=0;attempt<100 && !known;++attempt){frame(false);known=replaced(-.6f);}
    check(known,"the forgotten character is identified and replaced again");
    // As in the game: a character draw recorded while the application has a set 3 bound.
    bool kept=true;
    for(uint32_t i=0;i<5;++i){
        frame(false,true);
        kept&=shade_at(-.6f,.3f)==a_shade && shade_at(0,0)==b_shade && shade_at(.6f,.3f)==cross_shade && shade_at(0,.6f)==1 && shade_at(.6f,-.1f)==1 &&
              shade_at(.6f,-.6f)==0 && shade_at(-.6f,-.6f)==4 && shade_at(0,-.6f)==4;
    }
    check(kept,"with an application set 3 bound the character is still replaced, and the next four-set draw reads that set again");
    {
        // The replacement of a draw on a four-set pipeline puts its private set at slot 4 and leaves
        // the application's set 3 for the fragment shader: A's mesh at its replaced place, in the
        // color of the fourth set. Until the variant is built the application's draw stays.
        // In a material run this fragment shader has no profile and no sibling (it samples no
        // image): the mesh is replaced all the same and nothing of the application's is rebound.
        bool four_done=false;
        for(uint32_t attempt=0;attempt<400 && !four_done;++attempt){
            frame(false,false,true);
            four_done=shade_at(-.6f,.3f)==4 && shade_at(0,0)==b_shade && shade_at(.6f,.3f)==cross_shade && shade_at(0,.6f)==1 && shade_at(.6f,-.1f)==1 &&
                      shade_at(.6f,-.6f)==0 && shade_at(-.6f,-.6f)==0 && shade_at(0,-.6f)==0;
            if(!four_done)Sleep(5);
        }
        check(four_done,"a character draw on a four-set pipeline is replaced, with the private set above the application's four");
    }
    if(material){
        // A fragment shader without a profile that samples the profiled shader's texture at another
        // binding gets the rule of its own module (D-076): A's mesh in the mod's texture (5).
        // The application's texture on the mod's mesh would be 1, the application's own draw 1 at
        // the unreplaced place.
        bool derived=false;
        for(uint32_t attempt=0;attempt<400 && !derived;++attempt){
            frame(false,false,false,true);
            derived=shade_at(-.6f,.3f)==5 && shade_at(0,0)==b_shade && shade_at(.6f,.3f)==cross_shade && shade_at(0,.6f)==1 && shade_at(.6f,-.1f)==1 &&
                    shade_at(.6f,-.6f)==0 && shade_at(-.6f,-.6f)==0 && shade_at(0,-.6f)==0;
            if(!derived)Sleep(5);
        }
        check(derived,"an unprofiled fragment shader with the texture at another binding gets the mod's texture through the register rule");
    }
    // LOD 1 of component A: its palette is in another order and the package remaps it.
    // Merged bone 0 <- source 1 = (-.6,-.2), merged bone 1 <- source 0 = (-.2,.6).
    bool lod=false;
    for(uint32_t attempt=0;attempt<100 && !lod;++attempt){
        frame(true);
        lod=shade_at(-.2f,.3f)==a_shade && shade_at(.6f,.7f)==cross_shade && shade_at(0,0)==b_shade && shade_at(-.2f,.6f)==0 && shade_at(-.6f,-.5f)==0 &&
            shade_at(.6f,-.1f)==1 && shade_at(0,.6f)==1;
        if(!lod)Sleep(5);
    }
    check(lod,"LOD 1 section imports through its remap and drives both components");
    frame(false);frame(false);
    check(replaced(-.6f),"return to LOD 0 restores the unmapped palette");
    self_probe("{\"cmd\":\"m2\",\"enabled\":0}","character-disabled.json");
    frame(false);
    check(original(),"runtime disable returns every component to the application's draws");
    self_probe("{\"cmd\":\"m2\",\"enabled\":1}","character-enabled.json");
    bool again=false;
    for(uint32_t attempt=0;attempt<100 && !again;++attempt){frame(false);again=replaced(-.6f);}
    check(again,"runtime enable identifies the character again and resumes replacement");
    VK(vkResetCommandPool(g.device,g.pool,0));
    frame(false);
    check(replaced(-.6f),"command-pool reset recycles the shared snapshots");
    if(material){
        self_probe("{\"cmd\":\"m2\",\"enabled\":0}","character-resubmit-disabled.json");
        VK(g.submit(cmd));VK(vkQueueWaitIdle(g.queue));
        check(replaced(-.6f),"sealed shared bone and image snapshots survive command resubmission after runtime disable");
        self_probe("{\"cmd\":\"m2\",\"enabled\":1}","character-resubmit-enabled.json");
    }
    self_probe("{\"cmd\":\"status\"}","character-final.json");
    FILE* file=nullptr;fopen_s(&file,"pixels.rgba","wb");if(file){fwrite(readback.mapped,1,64*64*4,file);fclose(file);}
    // After everything above was counted by the built-in backend: the exchange.
    if(!g_module_path.empty()){
        // Other replacement code takes over while the scene runs. Its backend is made and staged at
        // once and becomes ready through the application's submissions; until it takes over, at the
        // end of a frame, the backend in use keeps replacing. The second round exchanges a backend
        // that itself came from a module.
        // Exchanges completed so far in this process; an earlier phase of the program may have made one.
        auto exchanges=[&]{
            const auto status=self_probe("{\"cmd\":\"status\"}","character-module-status.json");
            const auto at=status.find("\"generation\":");
            return at==std::string::npos?0u:static_cast<uint32_t>(atoi(status.c_str()+at+13));};
        const uint32_t before=exchanges();
        auto generation=[&](uint32_t n){return exchanges()==before+n;};
        auto exchange=[&](const std::string& path,const char* file){
            return self_probe(("{\"cmd\":\"module\",\"path\":\""+path+"\"}").c_str(),file);};
        bool settled=false;
        for(uint32_t attempt=0;attempt<100 && !settled;++attempt){frame(false);settled=replaced(-.6f);}
        check(settled,"the character is replaced before the exchange begins");
        for(uint32_t round=1;round<=2;++round){
            const auto answer=exchange(g_module_path,round==1?"character-module-1.json":"character-module-2.json");
            check(answer.find("\"ok\":true")!=std::string::npos && answer.find("\"staged\":true")!=std::string::npos,
                  "a module is loaded and its backend staged");
            bool took_over=false,kept_replacing=true;
            for(uint32_t attempt=0;attempt<600 && !took_over;++attempt){
                frame(false);
                took_over=generation(round);
                if(!took_over){kept_replacing&=replaced(-.6f);Sleep(5);}
            }
            check(took_over,"the staged backend takes over at the end of a frame");
            check(kept_replacing,"until then the backend in use keeps replacing");
            bool resumed=false;
            for(uint32_t attempt=0;attempt<400 && !resumed;++attempt){frame(false);resumed=replaced(-.6f);if(!resumed)Sleep(5);}
            check(resumed,"the new backend identifies the character and replaces it");
            if(round==1 && !g_bad_module_path.empty()){
                const auto refused=exchange(g_bad_module_path,"character-module-bad.json");
                check(refused.find("\"ok\":false")!=std::string::npos && refused.find("other structures")!=std::string::npos,
                      "a module built against other structures of the layer is refused");
                frame(false);
                check(replaced(-.6f) && generation(1),"and the backend in use is untouched by the refusal");
            }
        }
        // The module's backend goes through what the built-in one went through above.
        self_probe("{\"cmd\":\"m2\",\"enabled\":0}","character-module-disabled.json");
        frame(false);
        check(original(),"runtime disable works on the module's backend");
        self_probe("{\"cmd\":\"m2\",\"enabled\":1}","character-module-enabled.json");
        bool module_again=false;
        for(uint32_t attempt=0;attempt<100 && !module_again;++attempt){frame(false);module_again=replaced(-.6f);}
        check(module_again,"and so does runtime enable");
        // The probe's bone switch reaches the module's backend as it reaches the built-in one: the module
        // asks the host for the live value (BUG-055; it kept the copy its entry made).
        self_probe("{\"cmd\":\"set\",\"gpu_bones\":0}","character-module-cpu-bones-set.json");
        check(self_probe("{\"cmd\":\"status\"}","character-module-cpu-bones.json").find("\"bones\":{\"gpu\":false")!=std::string::npos,
              "the probe switches the module's bone import to the CPU");
        self_probe("{\"cmd\":\"set\",\"gpu_bones\":1}","character-module-gpu-bones-set.json");
        check(self_probe("{\"cmd\":\"status\"}","character-module-gpu-bones.json").find("\"bones\":{\"gpu\":true")!=std::string::npos,
              "and back to the device");
        VK(vkResetCommandPool(g.device,g.pool,0));
        frame(false);
        check(replaced(-.6f),"command-pool reset recycles the module's snapshots");
        self_probe("{\"cmd\":\"status\"}","character-swapped.json");
    }
    if (std::getenv("EFMI_VK_TEST_KEYS")) {
        auto write_text = [](const char* name, const std::string& contents) {
            std::ofstream out(name, std::ios::binary); out << contents;
            if (!out) throw std::runtime_error("Cannot update runtime test input");
        };
        auto press = [&](const char* keys) { write_text("keys.txt", keys); };
        auto number = [](const std::string& text, const char* name) {
            const auto at = text.find(std::string("\"") + name + "\":");
            return at == std::string::npos ? UINT32_MAX : uint32_t(strtoul(text.c_str() + at + strlen(name) + 3, nullptr, 10));
        };
        auto status = [&] { return self_probe("{\"cmd\":\"status\"}", "runtime-status.json"); };
        press("1 17 53\n"); // Ctrl+5, through the original dispatcher in this process only.
        frame(false);
        press("1\n");
        bool green = false;
        for (uint32_t i = 0; i < 100 && !green; ++i) { frame(false); green = shade_at(-.6f,.3f) == 8; if (!green) Sleep(5); }
        check(green && shade_at(0,0) == 6 && shade_at(.6f,-.1f) == 1, "original INI key and branch change GPU pixels and restore the following application draw");
        std::ifstream source("mod.ini", std::ios::binary);
        std::string original_text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
        original_text.erase(std::remove(original_text.begin(), original_text.end(), '\r'), original_text.end());
        source.close();
        write_text("mod.ini", original_text + "\n[Present]\n$tint = (\n");
        const auto before_reload = status();
        const auto first_generation = number(before_reload, "ini_generation");
        press("1 121\n"); frame(false); press("1\n");
        bool warned = false;
        for (uint32_t i = 0; i < 400 && !warned; ++i) { frame(false); warned = number(status(), "ini_generation") == first_generation + 1; if (!warned) Sleep(5); }
        // As the original loader does, the reload goes through: the broken line is reported and
        // skipped, everything else is the same, so the character stays green.
        const auto warned_status = status();
        check(warned && number(warned_status, "ini_reload_rejected") == 0 && number(warned_status, "diagnostics") >= 1 && shade_at(-.6f,.3f) == 8,
              "F10 with a broken line loads like the original: the line is reported and skipped, the character stays green");
        // No texture file changed: the new generation has the live one's images, read once (BUG-061).
        check(number(warned_status, "file_reads") == number(before_reload, "file_reads") &&
              number(warned_status, "declared") == number(before_reload, "declared"),
              "F10 with unchanged texture files reads none again and declares no second image");
        // Nor did a mesh file change: the new generation keeps the live mesh on the device (BUG-061).
        check(number(warned_status, "mesh_uploads") == number(before_reload, "mesh_uploads") &&
              number(warned_status, "meshes_kept") == number(before_reload, "meshes_kept") + 1,
              "F10 with unchanged mesh files uploads no mesh again and keeps the live one");
        const auto old_generation = first_generation + 1;
        write_text("mod.ini", original_text);
        std::ifstream texture("green.dds", std::ios::binary);
        const std::string texture_bytes((std::istreambuf_iterator<char>(texture)), std::istreambuf_iterator<char>());
        texture.close();
        write_text("green.dds", "broken DDS candidate");
        press("1 121\n"); frame(false); press("1\n");
        // As the original: the reload goes through, and a texture whose file does not load is a null
        // resource; the draw goes on and reads zero there (D-139; the candidate was refused before). Its
        // frames may still be the game's while the variant without the image builds (the frame gate,
        // D-097), so the pixel is waited for.
        bool reloaded = false;
        for (uint32_t i = 0; i < 400 && !reloaded; ++i) { frame(false); reloaded = number(status(), "ini_generation") == old_generation + 1; if (!reloaded) Sleep(5); }
        bool unbound = false;
        for (uint32_t i = 0; i < 200 && reloaded && !unbound; ++i) { unbound = shade_at(-.6f,.3f) == 7; if (!unbound) { frame(false); Sleep(5); } }
        check(reloaded && number(status(), "ini_reload_rejected") == 0 && unbound,
              "F10 with a texture file that does not load goes through as the original's: it binds nothing, the draw goes on");
        write_text("green.dds", texture_bytes);
        press("1 121\n"); frame(false); press("1\n");
        bool green_again = false;
        for (uint32_t i = 0; i < 400 && !green_again; ++i) {
            frame(false); green_again = number(status(), "ini_generation") == old_generation + 2 && shade_at(-.6f,.3f) == 8; if (!green_again) Sleep(5);
        }
        check(green_again, "F10 with the texture file repaired loads it again");
        const auto live_generation = old_generation + 2;
        auto changed = original_text;
        const auto alternate = changed.find("else\n ps-t1 = ResourceGreen\n");
        if (alternate == std::string::npos) throw std::runtime_error("Runtime fixture branch not found");
        changed.replace(alternate, strlen("else\n ps-t1 = ResourceGreen\n"), "else\n ps-t1 = ResourceRed\n");
        write_text("mod.ini", changed);
        // This records one more green command before F10 starts parsing. Keep
        // resubmitting that sealed recording throughout retirement of its owner.
        press("1 121\n"); frame(false);
        bool swapped = false, preserved = true;
        for (uint32_t i = 0; i < 600 && !swapped; ++i) {
            VK(g.submit(cmd)); VK(vkQueueWaitIdle(g.queue));
            preserved &= shade_at(-.6f,.3f) == 8;
            self_probe("{\"cmd\":\"m2\",\"advance\":1}", "runtime-advance.json");
            swapped = number(status(), "ini_generation") == live_generation + 1;
            if (!swapped) Sleep(5);
        }
        check(swapped && preserved, "F10 publishes a Ready generation while sealed commands keep the old mesh, bones and images");
        VK(g.submit(cmd)); VK(vkQueueWaitIdle(g.queue));
        check(shade_at(-.6f,.3f) == 8, "sealed old-generation command remains valid after F10 exchange");
        bool red = false;
        for (uint32_t i = 0; i < 400 && !red; ++i) { frame(false); red = shade_at(-.6f,.3f) == 5; if (!red) Sleep(5); }
        check(red && shade_at(0,0) == 6 && shade_at(.6f,-.1f) == 1, "new recordings use the reloaded INI and keep application state intact");
        for (int i = 0; i < 3; ++i) frame(false);
        check(number(status(), "ini_generation") == live_generation + 1 && status().find("\"ini_reload_pending\":false") != std::string::npos,
              "F10 held through the exchange does not retrigger in the new generation");
        press("1\n"); frame(false);
        self_probe("{\"cmd\":\"status\"}", "runtime-final.json");
        // The Mods folder (D-131): the drawn character's Mod is disabled and F10 pressed. 2.0.7 in the
        // game: the removed package was stopped twice at once, by its controller and with the generation
        // it left, and the second join's error ended the game in its presentation.
        char folder_scene[4] = {};
        if (GetEnvironmentVariableA("EFMI_VK_TEST_FOLDER_F10", folder_scene, sizeof(folder_scene)) && folder_scene[0] == '1') {
            if (!MoveFileExA("mod.ini", "DISABLED_mod.ini", 0)) throw std::runtime_error("Cannot disable the scene's Mod");
            const auto before_removal = number(status(), "reloads");
            press("1 121\n"); frame(false); press("1\n");
            bool removed = false;
            for (uint32_t i = 0; i < 600 && !removed; ++i) { frame(false); removed = number(status(), "reloads") == before_removal + 1; if (!removed) Sleep(5); }
            for (int i = 0; i < 100; ++i) { frame(false); Sleep(2); }
            check(removed, "F10 after the drawn character's Mod was disabled exchanges the folder generation without it");
            self_probe("{\"cmd\":\"status\"}", "runtime-removed.json");
            auto reloads_to = [&](uint32_t target, uint32_t tries) {
                bool reached = false;
                for (uint32_t i = 0; i < tries && !reached; ++i) { frame(false); reached = number(status(), "reloads") == target; if (!reached) Sleep(5); }
                return reached;
            };
            auto shade_becomes = [&](uint32_t shade) {
                bool seen = false;
                for (uint32_t i = 0; i < 400 && !seen; ++i) { frame(false); seen = shade_at(-.6f,.3f) == shade; if (!seen) Sleep(5); }
                return seen;
            };
            // Enabled again: drawn again, in the slot it had (BUG-068: 2.0.7 took a new one each time, 64 in all).
            if (!MoveFileExA("DISABLED_mod.ini", "mod.ini", 0)) throw std::runtime_error("Cannot enable the scene's Mod again");
            press("1 121\n"); frame(false); press("1\n");
            const bool back = reloads_to(before_removal + 2, 600);
            check(back && shade_becomes(5), "F10 after the Mod was enabled again draws it again");
            self_probe("{\"cmd\":\"status\"}", "runtime-reenabled.json");
            // Two F10 in a row, the INI edited between them: the second press is not lost (BUG-069: 2.0.7
            // dropped a press made while a reload loaded). The edit must be drawn.
            write_text("mod.ini", original_text);
            const auto before_pair = number(status(), "reloads");
            press("1 121\n"); frame(false); press("1\n"); frame(false); // released: the next press is a new one
            bool loading = false;
            for (uint32_t i = 0; i < 200 && !loading; ++i) {
                const auto text = status();
                loading = text.find("\"loading\":true") != std::string::npos || text.find("\"staged\":true") != std::string::npos;
                if (!loading) frame(false);
            }
            write_text("mod.ini", changed);
            press("1 121\n"); frame(false); press("1\n");
            const bool both = reloads_to(before_pair + 2, 800);
            check(loading && both && shade_becomes(5), "an F10 pressed while the last one loads reloads again, and its edit is drawn");
            // RabbitFX disabled, then enabled again (D-133, BUG-070): the gate follows the live generation.
            const char* const rabbit = "../RabbitFX -ENDMI-/RabbitFX.ini";
            const char* const rabbit_off = "../RabbitFX -ENDMI-/DISABLED_RabbitFX.ini";
            if (!MoveFileExA(rabbit, rabbit_off, 0)) throw std::runtime_error("Cannot disable the scene's RabbitFX");
            auto reloads = number(status(), "reloads");
            press("1 121\n"); frame(false); press("1\n");
            check(reloads_to(reloads + 1, 600), "F10 after RabbitFX was disabled exchanges the generation");
            for (int i = 0; i < 5; ++i) frame(false);
            self_probe("{\"cmd\":\"status\"}", "runtime-no-rabbitfx.json");
            if (!MoveFileExA(rabbit_off, rabbit, 0)) throw std::runtime_error("Cannot enable the scene's RabbitFX again");
            reloads = number(status(), "reloads");
            press("1 121\n"); frame(false); press("1\n");
            check(reloads_to(reloads + 1, 600), "F10 after RabbitFX was enabled again exchanges the generation");
            for (int i = 0; i < 20; ++i) { frame(false); Sleep(2); }
            self_probe("{\"cmd\":\"status\"}", "runtime-rabbitfx-back.json");
            // F10 with no generation loaded (docs/architecture-review-2.1.md, L-02): every character Mod is
            // disabled and F10 exchanges for a generation of none; the Mods are enabled again and F10 pressed.
            // The original reloads whatever it holds; the layer had no key binding left to take the press.
            const wchar_t* const others[] = {L"..\\other\\mod.ini", L"..\\Zeta\\mod.ini", L"..\\broken\\mod.ini", L"..\\\uff1aMod\\mod.ini"};
            auto every_mod = [&](bool enable) {
                for (const auto* ini : others) {
                    std::wstring off = ini; off.insert(off.rfind(L'\\') + 1, L"DISABLED_");
                    if (!MoveFileExW(enable ? off.c_str() : ini, enable ? ini : off.c_str(), 0)) throw std::runtime_error("Cannot switch a Mod of the scene");
                }
                if (!MoveFileExA(enable ? "DISABLED_mod.ini" : "mod.ini", enable ? "mod.ini" : "DISABLED_mod.ini", 0))
                    throw std::runtime_error("Cannot switch the scene's Mod");
            };
            every_mod(false);
            reloads = number(status(), "reloads");
            press("1 121\n"); frame(false); press("1\n");
            check(reloads_to(reloads + 1, 600), "F10 with every character Mod disabled exchanges for a generation of none");
            for (int i = 0; i < 5; ++i) frame(false);
            self_probe("{\"cmd\":\"status\"}", "runtime-no-mods.json");
            // The package that comes back is made to fail its activation once (L-01): its draws are the game's
            // meanwhile, not skipped for good, and the activation is tried again.
            SetEnvironmentVariableA("EFMI_VK_TEST_ACTIVATION_FAILS", "1");
            every_mod(true);
            press("1 121\n"); frame(false); press("1\n");
            check(reloads_to(reloads + 2, 600), "F10 with no generation loaded reloads the folder, as the original always does");
            bool failed = false;
            for (uint32_t i = 0; i < 400 && !failed; ++i) {
                frame(false);
                const auto count = number(status(), "activation_failed");
                failed = count != UINT32_MAX && count > 0;
                if (!failed) Sleep(5);
            }
            check(failed, "a package whose activation fails leaves its draws to the game");
            SetEnvironmentVariableA("EFMI_VK_TEST_ACTIVATION_FAILS", nullptr);
            bool again = false;
            for (uint32_t i = 0; i < 1500 && !again; ++i) { frame(false); again = shade_at(-.6f,.3f) == 5; if (!again) Sleep(5); }
            check(again, "the failed activation is tried again and the Mod is drawn");
            self_probe("{\"cmd\":\"status\"}", "runtime-reactivated.json");
        }
    }
}
