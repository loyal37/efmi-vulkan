// Native D3D11 observations and original INI conversion commands. Headless;
// all files and devices are private to this test, with no game/driver injection.
struct ImageNativeReference {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    HMODULE library=nullptr;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    ImageNativeReference(D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0){
        library=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(library!=nullptr,"Load source image conversion reference");
        const auto create=reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(library,"D3D11CreateDevice"));
        check(create!=nullptr,"Source image conversion device entry");
        check(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context)),"Create source image conversion reference");
    }
    ~ImageNativeReference(){context.Reset();device.Reset();if(library)FreeLibrary(library);}
    std::vector<uint8_t> clear(DXGI_FORMAT format,const std::array<float,4>& value,unsigned bytes){
        D3D11_TEXTURE2D_DESC desc{1,1,1,1,format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
        Ptr<ID3D11Texture2D> image,stage;Ptr<ID3D11RenderTargetView> view;
        check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&image)),"Create native integer render target");
        check(SUCCEEDED(device->CreateRenderTargetView(image.Get(),nullptr,&view)),"Create native integer RTV");
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&stage)),"Create native integer clear readback");
        context->ClearRenderTargetView(view.Get(),value.data());context->CopyResource(stage.Get(),image.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped)),"Read native integer clear");
        std::vector<uint8_t> out(bytes);memcpy(out.data(),mapped.pData,bytes);context->Unmap(stage.Get(),0);return out;
    }
    void depth_copy(const ImageDdsCase& c,const std::vector<std::vector<uint8_t>>& levels){
        D3D11_TEXTURE2D_DESC desc{c.width,c.height,c.mips,c.layers,c.bytes==2?DXGI_FORMAT_R16_TYPELESS:DXGI_FORMAT_R32_TYPELESS,
            {1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        for(UINT layer=0;layer<c.layers;++layer)for(UINT mip=0;mip<c.mips;++mip)
            data.push_back({levels[layer*c.mips+mip].data(),std::max(1u,c.width>>mip)*c.bytes,0});
        Ptr<ID3D11Texture2D> color,depth,stage;
        check(SUCCEEDED(device->CreateTexture2D(&desc,data.data(),&color)),"Native typeless color copy source");
        desc.BindFlags|=D3D11_BIND_DEPTH_STENCIL;
        check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&depth)),"Native same-family depth copy destination");
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&stage)),"Native same-family copy readback");
        context->CopyResource(depth.Get(),color.Get());context->CopyResource(stage.Get(),depth.Get());
        for(UINT layer=0;layer<c.layers;++layer)for(UINT mip=0;mip<c.mips;++mip){
            const auto index=layer*c.mips+mip,w=std::max(1u,c.width>>mip),h=std::max(1u,c.height>>mip);
            D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(stage.Get(),index,D3D11_MAP_READ,0,&mapped)),"Read native color/depth subresource");
            bool equal=true;for(UINT row=0;row<h;++row)equal=equal && !memcmp(static_cast<const uint8_t*>(mapped.pData)+row*mapped.RowPitch,
                levels[index].data()+row*w*c.bytes,w*c.bytes);
            context->Unmap(stage.Get(),index);check(equal,"Native color/depth family copy preserves every source bit");
        }
    }
};
static void image_ini_integer_rtv(Device& d,const std::filesystem::path& root){
    struct Format {const char* name;DXGI_FORMAT dxgi;unsigned bytes;const char* allocation=nullptr;};
    const Format formats[]={
#define I(name,bytes) {#name,DXGI_FORMAT_##name,bytes}
        I(R32G32B32A32_UINT,16),I(R32G32B32A32_SINT,16),I(R32G32_UINT,8),I(R32G32_SINT,8),I(R32_UINT,4),I(R32_SINT,4),
        I(R16G16B16A16_UINT,8),I(R16G16B16A16_SINT,8),I(R16G16_UINT,4),I(R16G16_SINT,4),I(R16_UINT,2),I(R16_SINT,2),
        I(R8G8B8A8_UINT,4),I(R8G8B8A8_SINT,4),I(R8G8_UINT,2),I(R8G8_SINT,2),I(R8_UINT,1),I(R8_SINT,1),I(R10G10B10A2_UINT,4),
#undef I
        {"R8G8B8A8_UINT",DXGI_FORMAT_R8G8B8A8_UINT,4,"R8G8B8A8_TYPELESS"},
        {"R32_SINT",DXGI_FORMAT_R32_SINT,4,"R32_TYPELESS"},
    };
    const float inf=std::numeric_limits<float>::infinity(),nan=std::numeric_limits<float>::quiet_NaN();
    struct Clear {const char* ini;std::array<float,4> values;};
    const Clear clears[]={
        {"0.5 1.5 2.5 3.5",{.5f,1.5f,2.5f,3.5f}},
        {"-1.75 -2.5 127.75 255.75",{-1.75f,-2.5f,127.75f,255.75f}},
        {"0xff800000 0x7f800000 0x7fc12345 0x80000000",{-inf,inf,nan,-0.0f}},
        {"16777216 16777218 -16777216 -16777218",{16777216.0f,16777218.0f,-16777216.0f,-16777218.0f}},
        {"-33554432 -33554436 -2147483648 4294967296",{-33554432.0f,-33554436.0f,-2147483648.0f,4294967296.0f}},
        {"65535.25 65535.75 32767.75 -32768.75",{65535.25f,65535.75f,32767.75f,-32768.75f}},
        {"int 1.25 2.75 -3.5 4.5",{1.25f,2.75f,-3.5f,4.5f}},
        {"0x00000001 0x80000001 0x7f7fffff 0xff7fffff",{std::numeric_limits<float>::denorm_min(),-std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()}},
        {"4294967040 2147483520 -2147483520 0",{4294967040.0f,2147483520.0f,-2147483520.0f,0}},
    };
    ImageNativeReference reference;
    // Independent fixed control: the native API truncates 1.5 and 3.5. A
    // round-to-even implementation would accidentally pass 0.5 and 2.5 alone.
    check(reference.clear(DXGI_FORMAT_R8G8B8A8_UINT,clears[0].values,4)==std::vector<uint8_t>({0,1,2,3}),"Native RTV fractions truncate toward zero");
    for(size_t index=0;index<std::size(formats);++index){const auto& format=formats[index];
        std::string source="namespace = images\n[Constants]\nglobal $phase = 0\n";
        source+=image_declaration("ResourceImage",format.allocation?format.allocation:format.name,"shader_resource render_target");
        source+="[ResourceView]\nformat = "+std::string(format.name)+"\n[Present]\nResourceView = ref ResourceImage\no0 = ref ResourceView\n";
        for(size_t phase=0;phase<std::size(clears);++phase)source+=(phase?"elif":"if")+std::string(" $phase == ")+std::to_string(phase)+
            "\n clear = o0 "+clears[phase].ini+"\n";
        source+="endif\no0 = null\nps-t0 = ref ResourceView\n$phase = $phase + 1\n";
        ImageIniFixture f(d,root/std::to_string(index),source);
        for(size_t phase=0;phase<std::size(clears);++phase){f.present(unsigned(phase+1));
            const auto expected=reference.clear(format.dxgi,clears[phase].values,format.bytes),bytes=f.read(0);
            const bool equal=bytes.size()==2*expected.size() && !memcmp(bytes.data(),expected.data(),expected.size()) &&
                !memcmp(bytes.data()+expected.size(),expected.data(),expected.size());
            if(!equal)printf("RTV mismatch format=%s alias=%d phase=%zu\n",format.name,format.allocation!=nullptr,phase);
            check(equal,"Original integer RTV clear matches native WARP bytes, including flags, signs, clamp, NaN and typeless views");
        }
        f.release();
    }
}
static PFN_vkMapMemory conversion_map_real=nullptr;
static PFN_vkQueueSubmit conversion_submit_real=nullptr;
static unsigned conversion_maps=0,conversion_submits=0;
static VKAPI_ATTR VkResult VKAPI_CALL conversion_map(VkDevice d,VkDeviceMemory m,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** data){
    ++conversion_maps;return conversion_map_real(d,m,offset,size,flags,data);
}
static VKAPI_ATTR VkResult VKAPI_CALL conversion_submit(VkQueue q,uint32_t count,const VkSubmitInfo* info,VkFence fence){
    ++conversion_submits;return conversion_submit_real(q,count,info,fence);
}
static void image_ini_color_depth(Device& d,const std::filesystem::path& root,bool wide){
    const ImageDdsCase c{"bridge",wide?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16_UNORM,3,7,3,1,3,2,wide?4u:2u,1};
    auto levels=write_image_dds(root/"Mods/image.dds",c);
    if(wide){
        // Arbitrary float bit patterns are valid D3D resource copies. Vulkan
        // must actually enable unrestricted depth for the color-to-D32 half.
        const uint32_t bits[]={0,0x80000000,0x3f800000,0x3f000000,0xbf000000,0x3fa00000,0x7f800000,0xff800000,0x7fc12345,1};
        std::fstream file(root/"Mods/image.dds",std::ios::in|std::ios::out|std::ios::binary);file.seekp(148);
        for(auto& level:levels){for(size_t i=0;i<level.size();i+=4)memcpy(level.data()+i,&bits[(i/4)%std::size(bits)],4);
            file.write(reinterpret_cast<const char*>(level.data()),level.size());}
        check(bool(file),"Write color/depth float bit-pattern fixture");
    }
    ImageNativeReference reference;reference.depth_copy(c,levels);
    auto original=d.resource.dispatch;conversion_map_real=original.MapMemory;conversion_submit_real=original.QueueSubmit;
    d.resource.dispatch.MapMemory=conversion_map;d.resource.dispatch.QueueSubmit=conversion_submit;
    ImageIniFixture f(d,root,R"ini(namespace = images
[Constants]
global $phase = 0
[ResourceSource]
filename = image.dds
bind_flags = shader_resource unordered_access
[ResourceDepth]
bind_flags = shader_resource depth_stencil
[ResourceBack]
bind_flags = shader_resource
[ResourceSnapshot]
bind_flags = shader_resource
[Present]
if $phase == 1
 clear = ResourceSource 0x3e800000
endif
ResourceDepth = copy ResourceSource
ResourceBack = copy ResourceDepth
if $phase == 0
 ResourceSnapshot = copy ResourceBack
endif
ps-t0 = ref ResourceSource
ps-t1 = ref ResourceBack
ps-t2 = ref ResourceSnapshot
$phase = $phase + 1
)ini");
    d.resource.dispatch=original;f.present();
    for(UINT layer=0;layer<c.layers;++layer)for(UINT mip=0;mip<c.mips;++mip)
        check(f.read(1,mip,layer)==levels[layer*c.mips+mip],"Original color/depth/color copies preserve every bit of every mip and array layer");
    const auto resident=f.budgets.resident->used();
    conversion_maps=conversion_submits=0;
    f.backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
    check(f.ini->present({},2,0,f.why),"Second color/depth conversion frame");
    check(conversion_maps==0 && conversion_submits==0 && !f.budgets.staging->used(),"Color/depth transfers record without CPU maps, staging, implicit submissions or waits");
    check(f.budgets.resident->used()>=resident,"Color/depth generations retain their reusable scratch and copy allocations");
    auto ticket=f.backend->finish();check(conversion_submits==1,"Only explicit finish submits the conversion batch");ticket->wait();ticket.reset();
    const auto reused=f.budgets.resident->used();
    for(UINT layer=0;layer<c.layers;++layer)for(UINT mip=0;mip<c.mips;++mip){
        const auto original_bytes=levels[layer*c.mips+mip];auto expected=original_bytes;
        if(!mip){if(wide){const uint32_t bits=0x3e800000;for(size_t i=0;i<expected.size();i+=4)memcpy(expected.data()+i,&bits,4);}
            else std::fill(expected.begin(),expected.end(),0);}
        check(f.read(1,mip,layer)==expected && f.read(2,mip,layer)==original_bytes,"Repeated color/depth bridge preserves untouched mips and independent old copies");
    }
    f.present(3);check(f.budgets.resident->used()==reused,"Repeated equal-size color/depth copies reuse allocation capacity");f.release();
}
static void image_color_depth_regions(Device& d,bool wide){
    const VkFormat narrow_formats[]={VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_UINT,VK_FORMAT_R16_SINT,VK_FORMAT_R16_SFLOAT};
    const VkFormat wide_formats[]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT};
    const auto* formats=wide?wide_formats:narrow_formats;const auto count=wide?std::size(wide_formats):std::size(narrow_formats);
    const unsigned unit=wide?4:2;
    for(size_t format=0;format<count;++format){gpu::ResourceBudgets budgets;
        {gpu::ResourceCommands c(d.resource,budgets);gpu::ImageDescription desc;desc.format=formats[format];desc.extent={7,5,1};desc.layers=3;
            auto color=c.create_image(desc),back=c.create_image(desc);desc.format=wide?VK_FORMAT_D32_SFLOAT:VK_FORMAT_D16_UNORM;auto depth=c.create_image(desc);
            std::vector<uint8_t> source(7*5*3*unit),expected_back(source.size(),0),expected_depth(source.size());
            for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*31+i/7);
            c.write(color,image_region(color,0,0,3),source.data(),source.size());
            c.write(back,image_region(back,0,0,3),expected_back.data(),expected_back.size());
            c.clear(depth,depth->full_range(),VkClearDepthStencilValue{wide?.25f:1.0f,0});
            const uint32_t sentinel=wide?0x3e800000:0xffff;
            for(size_t i=0;i<expected_depth.size();i+=unit)memcpy(expected_depth.data()+i,&sentinel,unit);
            VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2};region.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,2};
            region.srcOffset={1,1,0};region.dstOffset={2,2,0};region.extent={3,2,1};c.copy_color_depth(color,depth,region);
            std::swap(region.srcSubresource,region.dstSubresource);region.srcOffset={2,2,0};region.dstOffset={3,1,0};c.copy_color_depth(depth,back,region);
            for(unsigned layer=0;layer<2;++layer)for(unsigned row=0;row<2;++row)for(unsigned col=0;col<3;++col){
                const auto input=source.data()+((layer*5+row+1)*7+col+1)*unit;
                memcpy(expected_depth.data()+(((layer+1)*5+row+2)*7+col+2)*unit,input,unit);
                memcpy(expected_back.data()+((layer*5+row+1)*7+col+3)*unit,input,unit);
            }
            check(c.readback(d.queue,depth,image_region(depth,0,0,3,VK_IMAGE_ASPECT_DEPTH_BIT))==expected_depth,"Partial color/depth copies preserve unselected depth texels and layers");
            check(c.readback(d.queue,back,image_region(back,0,0,3))==expected_back,"Partial depth/color copies preserve unselected color texels and all format bits");
            check(c.readback(d.queue,color,image_region(color,0,0,3))==source,"Color/depth bridge leaves its source unchanged");
            refuses([&]{c.copy_color_depth(color,depth,region);},"Color/depth bridge rejects mismatched source aspects");
        }
        check(!budgets.resident->used() && !budgets.staging->used(),"Partial conversion releases every image and resident scratch allocation");
    }
}
static void image_depth_conversion(Device& d,const std::filesystem::path& root){
    image_ini_color_depth(d,root/"r16",false);
    image_color_depth_regions(d,false);
    {
        gpu::ResourceBudgets budgets;
        {gpu::ResourceCommands c(d.resource,budgets);gpu::ImageDescription desc;desc.format=VK_FORMAT_R32_SFLOAT;desc.extent={3,2,1};
            auto color=c.create_image(desc);desc.format=VK_FORMAT_D32_SFLOAT;auto depth=c.create_image(desc);
            auto r=image_copy_region(color);r.dstSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            refuses([&]{c.copy_color_depth(color,depth,r);},"Opaque color-to-D32 copy refuses without enabled unrestricted-depth extension");
            c.clear(depth,depth->full_range(),VkClearDepthStencilValue{.75f,0});std::swap(r.srcSubresource,r.dstSubresource);
            c.copy_color_depth(depth,color,r);const auto bytes=c.readback(d.queue,color,image_region(color));
            for(size_t i=0;i<bytes.size();i+=4)check(value(bytes,i)==.75f,"D32-to-color bit copy works without unrestricted-depth extension");
            // Pitched uploads ignore invalid padding but must reject an invalid
            // copied depth texel before any command is recorded.
            auto upload=image_region(depth,0,0,1,VK_IMAGE_ASPECT_DEPTH_BIT);upload.bufferRowLength=4;upload.bufferImageHeight=3;
            std::vector<float> values(7,.5f);values[3]=std::numeric_limits<float>::quiet_NaN();
            c.write(depth,upload,values.data(),values.size()*4);
            auto submitted=c.submit(d.queue);submitted->wait();submitted.reset();
            values[5]=-1.0f;refuses([&]{c.write(depth,upload,values.data(),values.size()*4);},"Float-depth host upload rejects an out-of-range copied texel");
            values[5]=std::numeric_limits<float>::quiet_NaN();refuses([&]{c.write(depth,upload,values.data(),values.size()*4);},"Float-depth host upload rejects NaN without the extension");
            check(!c.submit(d.queue),"Rejected depth host uploads record no commands");
            desc.format=VK_FORMAT_D16_UNORM;auto narrow=c.create_image(desc);r=image_copy_region(color);r.dstSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            refuses([&]{c.copy_color_depth(color,narrow,r);},"Color/depth bridge refuses different texel widths");
        }
        check(!budgets.resident->used() && !budgets.staging->used(),"Color/depth bridge scratch retires with its owned executor");
    }
    {
        ImageIniFixture f(d,root/"missing-extension","namespace = images\n"+image_declaration("ResourceSource","R32_FLOAT")+R"ini(
[ResourceDepth]
bind_flags = shader_resource depth_stencil
[Present]
clear = ResourceSource 0.25
ResourceDepth = copy_desc ResourceSource
ps-t0 = ref ResourceDepth
ResourceDepth = copy ResourceSource
)ini");
        f.backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        const bool presented=f.ini->present({},1,0,f.why);
        check(f.srv(0).image && f.srv(0).image_view,"INI color-to-D32 copy_desc creates the description without needing a transfer extension");
        declined(*f.ini,*f.backend,presented,"enabled VK_EXT_depth_range_unrestricted","INI color-to-D32 copy says the extension that is not enabled and copies nothing");f.release();
    }
    {
        gpu::ResourceBudgets budgets;
        auto c=std::make_unique<gpu::ResourceCommands>(d.resource,budgets);gpu::ImageDescription desc;desc.format=VK_FORMAT_R16_UINT;desc.extent={3,2,1};
        auto color=c->create_image(desc);desc.format=VK_FORMAT_D16_UNORM;auto depth=c->create_image(desc);
        VkImageCopy r=image_copy_region(color);r.dstSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        const auto remaining=budgets.resident->limit()-budgets.resident->used();
        check(budgets.resident->acquire(remaining),"Reserve remaining conversion budget");
        refuses([&]{c->copy_color_depth(color,depth,r);},"Color/depth scratch observes the shared resident budget");
        check(budgets.resident->used()==budgets.resident->limit() && !c->submit(d.queue),"Failed scratch allocation leaks no budget and records no commands");budgets.resident->release(remaining);
        VkClearColorValue clear{};clear.uint32[0]=0x1234;c->clear(color,color->full_range(),clear);c->copy_color_depth(color,depth,r);
        std::weak_ptr<gpu::ImageResource> held_color=color,held_depth=depth;auto ticket=c->submit(d.queue);color.reset();depth.reset();c.reset();
        check(!held_color.expired() && !held_depth.expired() && budgets.resident->used()>0,"Conversion submission retains both images and scratch after executor retirement");
        ticket->wait();ticket.reset();check(held_color.expired() && held_depth.expired() && !budgets.resident->used() && !budgets.staging->used(),"Completed conversion ticket releases its last image and scratch owners");
    }
    {Device unrestricted;unrestricted.init(true);image_ini_color_depth(unrestricted,root/"r32",true);image_color_depth_regions(unrestricted,true);}
}
