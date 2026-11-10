// Original INI -> owned Vulkan images. Pixel assertions include independent
// source-API observations; D3D11 is loaded only by this headless test process.
#include <wrl/client.h>
static void image_source_reference() {
    using Microsoft::WRL::ComPtr;
    const auto library=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(library!=nullptr,"System D3D11 reference loader");
    {
        const auto create=reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(library,"D3D11CreateDevice"));
        check(create!=nullptr,"System D3D11 reference entry");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
        check(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context)),"Headless WARP reference device");
        for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R8G8B8A8_TYPELESS}){
            D3D11_TEXTURE2D_DESC desc{2,2,1,1,format,{1,0},D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET,0,0};
            ComPtr<ID3D11Texture2D> texture;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture)),"Source reference image creation");
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=UINT(-1);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
            D3D11_RENDER_TARGET_VIEW_DESC rtv{};rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
            ComPtr<ID3D11ShaderResourceView> s;ComPtr<ID3D11UnorderedAccessView> u;ComPtr<ID3D11RenderTargetView> r;
            const auto hs=device->CreateShaderResourceView(texture.Get(),&srv,&s);
            const auto hu=device->CreateUnorderedAccessView(texture.Get(),&uav,&u);
            const auto hr=device->CreateRenderTargetView(texture.Get(),&rtv,&r);
            const bool typed=format!=DXGI_FORMAT_R8G8B8A8_TYPELESS;
            check(SUCCEEDED(hs)==typed && SUCCEEDED(hu)==typed && SUCCEEDED(hr)==typed,"Native UNKNOWN image view inherits typed resource format and refuses typeless format");
            if(typed){s->GetDesc(&srv);u->GetDesc(&uav);r->GetDesc(&rtv);
                check(srv.Format==format && uav.Format==format && rtv.Format==format,"Native image view reports its inferred typed format");}
            for(auto other:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UINT,DXGI_FORMAT_R32_FLOAT}){
                s.Reset();srv.Format=other;const auto result=device->CreateShaderResourceView(texture.Get(),&srv,&s);
                const bool compatible=format==other || (format==DXGI_FORMAT_R8G8B8A8_TYPELESS && other!=DXGI_FORMAT_R32_FLOAT);
                check(SUCCEEDED(result)==compatible,"Native view format must equal a typed resource or belong to its typeless DXGI family");
            }
        }
    }
    FreeLibrary(library);
}
struct ImageIniFixture {
    using T=ResourceCopyTargetType;
    Device& device;
    gpu::ResourceBudgets budgets;
    std::shared_ptr<frontend::GpuResourceRuntime> backend;
    std::shared_ptr<frontend::IniRuntime> ini;
    std::string why;
    ImageIniFixture(Device& d,const std::filesystem::path& root,const std::string& text):device(d){
        std::filesystem::create_directories(root/"Mods");write(root/"Mods/image.ini",text.c_str());
        frontend::RuntimeConfig config{root,{},root/"settings","images",root/"Mods"};
        backend=std::make_shared<frontend::GpuResourceRuntime>(d.resource,budgets);
        ini=frontend::IniRuntime::load(config,backend,why);
        if(!ini)throw std::runtime_error("Load image fixture: "+why);++checks;
    }
    void present(unsigned frame=1){
        backend->begin(device.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        if(!ini->present({},frame,0,why))throw std::runtime_error("Image fixture Present: "+why);++checks;
        auto ticket=backend->finish();if(ticket)ticket->wait();
    }
    frontend::GpuResourceBinding srv(unsigned slot){return backend->binding(L'p',T::SHADER_RESOURCE,slot);}
    std::vector<uint8_t> read(unsigned slot,UINT mip=0,UINT layer=0,UINT layers=1,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT){
        const auto image=srv(slot).image;check(bool(image),"INI image is bound");
        return backend->commands().readback(device.queue,image,image_region(image,mip,layer,layers,aspect));
    }
    void variable(const char* name,float expected){const auto values=ini->status().variables;const auto key=std::string("$\\images\\")+name;
        const auto found=values.find(key);if(found==values.end() || found->second!=expected)
            throw std::runtime_error("Image variable "+key+" expected "+std::to_string(expected)+" got "+(found==values.end()?"missing":std::to_string(found->second)));++checks;
    }
    void release(){ini.reset();backend.reset();check(!budgets.resident->used() && !budgets.staging->used(),"Original image generation releases all allocation budgets");}
};
static void image_ini_declarations(Device& d,const std::filesystem::path& root) {
    ImageIniFixture f(d,root,R"ini(namespace = images
[Constants]
global $declared_width = 0
global $width = 0
global $height = 0
global $array = 0
global $mips = 0
global $format = 0
global $stride = 0
global $size = 0
global $offset = 0
global $identity = 0
global $flags = 0
global $phase = 0
[ResourceSource]
type = Texture2D
width = 8
height = 2
width_multiply = 0.5
array = 2
mips = 2
msaa = 1
format = R32G32B32A32_FLOAT
bind_flags = shader_resource unordered_access render_target
[ResourceCopy]
bind_flags = shader_resource unordered_access
[ResourceAlias]
[ResourceDescription]
bind_flags = shader_resource unordered_access
[ResourceLine]
type = Texture1D
width = 3
height = -2
depth = -3
msaa = -4
msaa_quality = -5
height_multiply = -1
array = 2
mips = 1
format = R8G8B8A8_UINT
bind_flags = shader_resource unordered_access
[Present]
if $phase == 0
 clear = ResourceSource 1.25 2.5 3.75 4.5
 ResourceCopy = copy ResourceSource
 ResourceAlias = ref ResourceSource
 clear = ResourceAlias 5.0
 ResourceDescription = copy_desc ResourceSource
 clear = ResourceDescription 6.0
 ps-t0 = ref ResourceSource
 ps-t1 = ref ResourceCopy
 ps-t2 = ref ResourceAlias
 ps-t3 = ref ResourceDescription
 clear = ps-t0 0.0
 clear = ResourceLine 1 257 511 3
 ps-t4 = ref set_viewport ResourceLine
 $declared_width = ResourceSource->width
 $width = ps-t0->width
 $height = ps-t0->height
 $array = ps-t0->array
 $mips = ps-t0->mips
 $format = ps-t0->format
 $stride = ps-t0->stride
 $size = ps-t0->size
 $offset = ps-t0->offset
 $identity = @ps-t0 == @ps-t2
 $flags = ps-t0->bindflags
endif
$phase = $phase + 1
)ini");
    f.present();
    check(f.srv(0).image==f.srv(2).image && f.srv(0).image!=f.srv(1).image,"Image references share allocations; copies own independent allocations");
    check(f.srv(0).image_view->range().levelCount==2 && f.srv(0).image_view->range().layerCount==2,"Original image SRV includes all mips and layers");
    auto bytes=f.read(0,0,0,2);for(size_t i=0;i<bytes.size();i+=4)check(value(bytes,i)==5.0f,"SRV clear is a no-op; alias clear changes source image");
    bytes=f.read(1,0,0,2);const float expected[]={1.25f,2.5f,3.75f,4.5f};
    for(size_t i=0;i<bytes.size();i+=4)check(value(bytes,i)==expected[(i/4)%4],"Image copy preserves all cleared channels across array layers");
    bytes=f.read(3,0,0,2);for(size_t i=0;i<bytes.size();i+=4)check(value(bytes,i)==6.0f,"Description-copy image can be cleared independently");
    bytes=f.read(4,0,0,2);const uint8_t integers[]={1,1,255,3};for(size_t i=0;i<bytes.size();++i)check(bytes[i]==integers[i%4],"Integer image clear narrows channel values without float conversion");
    check(f.backend->viewport().Width==3 && f.backend->viewport().Height==0,"Original 1D set_viewport preserves zero height");
    for(auto pair:{std::pair<const char*,float>{"declared_width",8},{"width",4},{"height",2},{"array",2},{"mips",2},
        {"format",2},{"stride",-3},{"size",-3},{"offset",-3},{"identity",1},{"flags",168}})f.variable(pair.first,pair.second);
    f.release();
}
static std::string image_declaration(const char* name,const char* format,const char* flags="shader_resource unordered_access"){
    return std::string("[")+name+"]\ntype = Texture2D\nwidth = 2\nheight = 1\narray = 1\nmips = 1\nmsaa = 1\nformat = "+format+"\nbind_flags = "+flags+"\n";
}
static void image_ini_clears(Device& d,const std::filesystem::path& root){
    std::string source="namespace = images\n";
    source+=image_declaration("ResourceBits","R32G32B32A32_FLOAT");
    source+=image_declaration("ResourceUnorm","R8G8B8A8_UNORM");
    source+=image_declaration("ResourceHalf","R16G16_FLOAT");
    source+=image_declaration("ResourcePacked","R11G11B10_FLOAT");
    source+=image_declaration("ResourceTen","R10G10B10A2_UNORM");
    source+=image_declaration("ResourceBgra","B8G8R8A8_UNORM","shader_resource render_target");
    source+=image_declaration("ResourceSrgb","R8G8B8A8_TYPELESS","shader_resource render_target");
    source+=image_declaration("ResourceSigned","R8G8B8A8_SINT");
    source+=R"ini([ResourceSrgbView]
format = R8G8B8A8_UNORM_SRGB
[Present]
clear = ResourceBits 0x7fc01234 0x80000000 0x00000001 0xffc54321
clear = ResourceUnorm 0x01 0xff 0x100 0x1ff
clear = ResourceHalf 0x3c00 0xbc00
clear = ResourcePacked 0x7ff 0x123 0x3ff
clear = ResourceTen 0x3ff 0x400 0x123 0x7
clear = ResourceBgra 1.0 0.6 0.2 0.0
clear = ResourceSigned 0xfffffffe 0x81 0x7f 0x123
ResourceSrgbView = ref ResourceSrgb
o0 = ref ResourceSrgbView
clear = o0 0.5 0.5 0.5 0.25
o0 = null
ps-t0 = ref ResourceBits
ps-t1 = ref ResourceUnorm
ps-t2 = ref ResourceHalf
ps-t3 = ref ResourcePacked
ps-t4 = ref ResourceTen
ps-t5 = ref ResourceBgra
ps-t6 = ref ResourceSrgbView
ps-t7 = ref ResourceSigned
)ini";
    ImageIniFixture f(d,root,source);f.present();
    const uint32_t nan_bits[]={0x7fc01234,0x80000000,1,0xffc54321};auto bytes=f.read(0);
    check(bytes.size()==32 && !memcmp(bytes.data(),nan_bits,16) && !memcmp(bytes.data()+16,nan_bits,16),"Integer UAV clear preserves NaN payloads, negative zero and denormal bits in a float image");
    const std::array<uint8_t,4> expected[]={{{1,255,0,255}},{{0,0x3c,0,0xbc}},
        {{0xff,0x1f,0xc9,0xff}},{{0xff,0x03,0x30,0xd2}},{{51,153,255,0}},{{188,188,188,64}},{{254,129,127,35}}};
    for(UINT slot=1;slot<=7;++slot){bytes=f.read(slot);if(bytes.size()>=4 && memcmp(bytes.data(),expected[slot-1].data(),4))
        printf("Image clear slot %u: %02x %02x %02x %02x\n",slot,bytes[0],bytes[1],bytes[2],bytes[3]);
        check(bytes.size()==8 && !memcmp(bytes.data(),expected[slot-1].data(),4) &&
        !memcmp(bytes.data()+4,expected[slot-1].data(),4),"INI clear matches packed channel bytes including signed, normalized, half, BGRA and sRGB formats");}
    check(f.srv(6).image->description().format==VK_FORMAT_R8G8B8A8_UNORM && f.srv(6).image_view->format()==VK_FORMAT_R8G8B8A8_SRGB,
        "Typeless allocation and sRGB view retain distinct formats");f.release();
}
static void image_ini_depth_resolve(Device& d,const std::filesystem::path& root){
    ImageIniFixture f(d,root,R"ini(namespace = images
[Constants]
global $phase = 0
[ResourceDepth]
type = Texture2D
width = 2
height = 2
array = 2
mips = 1
msaa = 1
format = R24G8_TYPELESS
bind_flags = shader_resource depth_stencil
[ResourceDepthCopy]
bind_flags = shader_resource
[ResourceMS]
type = Texture2D
width = 2
height = 2
array = 2
mips = 1
msaa = 4
format = R8G8B8A8_UNORM
bind_flags = shader_resource render_target
[ResourceResolved]
bind_flags = shader_resource
[Present]
od = ref ResourceDepth
if $phase == 0
 clear = ResourceDepth 0.25 7
 od = ref ResourceDepth
 clear = od depth stencil 0.5 33
 ResourceDepthCopy = copy ResourceDepth
 ps-t1 = ref ResourceDepthCopy
 clear = ResourceMS 0.2 0.4 0.6 1.0
 ResourceResolved = resolve_msaa set_viewport ResourceMS
 ps-t2 = ref ResourceResolved
elif $phase == 1
 clear = od stencil 11
elif $phase == 2
 clear = od depth -2
else
 clear = od depth 2
endif
od = null
ps-t0 = ref ResourceDepth
$phase = $phase + 1
)ini");
    f.present();check(f.srv(0).image_view->range().aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT,"Original depth-compatible SRV selects only the depth aspect");
    auto bytes=f.read(0,0,0,2,VK_IMAGE_ASPECT_DEPTH_BIT);for(size_t i=0;i<bytes.size();i+=4){uint32_t word;memcpy(&word,bytes.data()+i,4);
        check((word&0xffffff)==0x800000,"Original depth/stencil flag priority clears depth only when both flags appear");}
    bytes=f.read(0,0,0,2,VK_IMAGE_ASPECT_STENCIL_BIT);check(std::all_of(bytes.begin(),bytes.end(),[](auto x){return x==7;}),"Explicit depth priority preserves stencil");
    check(f.read(1,0,0,2,VK_IMAGE_ASPECT_DEPTH_BIT)==f.read(0,0,0,2,VK_IMAGE_ASPECT_DEPTH_BIT) &&
          f.read(1,0,0,2,VK_IMAGE_ASPECT_STENCIL_BIT)==bytes,"Depth/stencil copy retains both aspects even with a depth-only SRV");
    bytes=f.read(2,0,0,2);const uint8_t expected[]={51,102,153,255};for(size_t i=0;i<bytes.size();++i)check(bytes[i]==expected[i%4],"Original MSAA resolve copies every array layer into a single-sample image");
    check(f.srv(2).image->description().samples==VK_SAMPLE_COUNT_1_BIT && f.backend->viewport().Width==2 && f.backend->viewport().Height==2,"Resolve and set_viewport use destination description");
    f.present(2);bytes=f.read(0,0,0,2,VK_IMAGE_ASPECT_STENCIL_BIT);check(std::all_of(bytes.begin(),bytes.end(),[](auto x){return x==11;}),"Explicit stencil clear uses its first value");
    check(f.read(0,0,0,2,VK_IMAGE_ASPECT_DEPTH_BIT)==f.read(1,0,0,2,VK_IMAGE_ASPECT_DEPTH_BIT),"Stencil clear preserves source depth and independent depth copy");
    for(unsigned frame=3;frame<=4;++frame){f.present(frame);bytes=f.read(0,0,0,2,VK_IMAGE_ASPECT_DEPTH_BIT);
        for(size_t i=0;i<bytes.size();i+=4){uint32_t word;memcpy(&word,bytes.data()+i,4);check((word&0xffffff)==(frame==3?0:0xffffff),"INI depth clear clamps to the original zero-to-one range");}}
    f.release();
}
struct ImageDdsCase {const char* name;DXGI_FORMAT format;UINT dimension,width,height,depth,mips,layers,bytes,block;bool cube=false,legacy=false;};
static std::vector<std::vector<uint8_t>> write_image_dds(const std::filesystem::path& path,const ImageDdsCase& c){
    // Independent DDS fixture encoding, not the production DirectXTK decoder.
    std::array<uint32_t,37> header{};header[0]=0x20534444;header[1]=124;header[2]=0x21007|(c.dimension==4?0x800000:0);
    header[3]=c.height;header[4]=c.width;header[6]=c.depth;header[7]=c.mips;
    header[19]=32;header[20]=4;header[21]=0x30315844;header[27]=0x401008;
    header[32]=c.format;header[33]=c.dimension;header[34]=c.cube?4:0;header[35]=c.cube?c.layers/6:c.layers;
    if(c.legacy){header[20]=0x41;header[21]=0;header[22]=32;header[23]=0x00ff0000;header[24]=0x0000ff00;header[25]=0x000000ff;header[26]=0xff000000;}
    std::filesystem::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()),(c.legacy?32:37)*4);
    std::vector<std::vector<uint8_t>> levels;
    for(UINT layer=0;layer<c.layers;++layer)for(UINT mip=0;mip<c.mips;++mip){
        const auto w=std::max(1u,c.width>>mip),h=std::max(1u,c.height>>mip),z=std::max(1u,c.depth>>mip);
        std::vector<uint8_t> bytes(size_t((w+c.block-1)/c.block)*((h+c.block-1)/c.block)*z*c.bytes);
        for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(19+layer*31+mip*17+i*3);
        // The ordinary device has no unrestricted-depth extension. Packed D32
        // fixtures therefore contain valid depth; stencil/padding stay distinct.
        if(c.format==DXGI_FORMAT_R32G8X24_TYPELESS)for(size_t i=0;i<bytes.size();i+=8){
            const float depth=float((i/8+layer+mip)%5)*0.25f;memcpy(bytes.data()+i,&depth,4);
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());levels.push_back(std::move(bytes));
    }
    check(bool(file),"Write isolated DDS subresources");return levels;
}
static void image_ini_dds(Device& d,const std::filesystem::path& root){
    const ImageDdsCase cases[]={
        {"line",DXGI_FORMAT_R16_UINT,2,8,1,1,4,3,2,1},
        {"array",DXGI_FORMAT_R8G8B8A8_UNORM,3,7,5,1,3,2,4,1},
        {"volume",DXGI_FORMAT_R32_FLOAT,4,4,3,2,3,1,4,1},
        {"cubes",DXGI_FORMAT_R8G8B8A8_UNORM,3,4,4,1,3,12,4,1,true},
        {"bc1",DXGI_FORMAT_BC1_UNORM,3,7,5,1,3,2,8,4},
        {"bc7",DXGI_FORMAT_BC7_UNORM,3,8,8,1,4,1,16,4},
        {"legacy",DXGI_FORMAT_B8G8R8A8_UNORM,3,4,2,1,3,1,4,1,false,true},
    };
    for(const auto& c:cases){const auto directory=root/c.name;const auto levels=write_image_dds(directory/"Mods/image.dds",c);
        ImageIniFixture f(d,directory,R"ini(namespace = images
[Constants]
global $width = 0
global $height = 0
global $array = 0
global $mips = 0
global $declared_width = 0
global $declared_format = 0
[ResourceLoaded]
filename = image.dds
width = 99
format = R32_FLOAT
bind_flags = shader_resource
misc_flags = texturecube
[ResourceCopy]
bind_flags = shader_resource
[Present]
ps-t0 = ref ResourceLoaded
ResourceCopy = copy ResourceLoaded
ps-t1 = ref ResourceCopy
$width = ps-t0->width
$height = ps-t0->height
$array = ps-t0->array
$mips = ps-t0->mips
$declared_width = ResourceLoaded->width
$declared_format = ResourceLoaded->format
)ini");
        f.present();f.variable("width",float(c.width));f.variable("height",float(c.height));f.variable("array",c.dimension==4?-4.0f:float(c.layers));
        f.variable("mips",float(c.mips));f.variable("declared_width",99);f.variable("declared_format",41);
        for(UINT layer=0;layer<c.layers;++layer)for(UINT mip=0;mip<c.mips;++mip){const auto& expected=levels[layer*c.mips+mip];
            check(f.read(0,mip,layer)==expected,"Original DDS file loads every layer, mip and volume slice without applying declaration extent/format overrides");
            check(f.read(1,mip,layer)==expected,"Original image copy preserves complete DDS subresources");}
        check(f.srv(0).image_view->range().levelCount==c.mips && f.srv(0).image_view->range().layerCount==c.layers,"DDS view spans every declared subresource");
        if(c.cube)check(f.srv(0).image_view->type()==VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,"Original cube descriptor uses cube-array view for complete face arrays");
        if(c.dimension==4)check(f.srv(0).image->description().type==VK_IMAGE_TYPE_3D && f.srv(0).image_view->type()==VK_IMAGE_VIEW_TYPE_3D,"Volume remains a real 3D allocation and view");f.release();
    }
}
static void image_ini_file_cache(Device& d,const std::filesystem::path& root){
    const ImageDdsCase c{"array",DXGI_FORMAT_R8G8B8A8_UNORM,3,4,2,1,3,2,4,1};
    const auto levels=write_image_dds(root/"Mods/image.dds",c);
    ImageIniFixture f(d,root,R"ini(namespace = images
[Constants]
global $phase = 0
global $range_format = 0
[ResourceLoaded]
filename = image.dds
bind_flags = shader_resource unordered_access
[ResourceCopy]
bind_flags = shader_resource unordered_access
[ResourceNull]
[PoolImages]
pool_size = 2
[Present]
if $phase == 0
 ResourceCopy = copy ResourceLoaded
 ps-t1 = ref ResourceCopy
 PoolImages[*] = ref ResourceLoaded
 ps-t[4:5] = PoolImages[0:1]
 $range_format = ps-t4->format
 clear = ResourceLoaded 0.2 0.4 0.6 1.0
 ps-t0 = ref ResourceLoaded
else
 ResourceCopy = ref ResourceLoaded
 ps-t2 = ref ResourceCopy
 ResourceCopy = copy_desc ResourceLoaded
 ps-t3 = ref ResourceCopy
 PoolImages[0:1] = ref ps-t[4:5]
 ps-t[6:7] = PoolImages[0:1]
 ResourceCopy = null
 ResourceCopy = unless_null copy ResourceNull
 ps-t8 = ref ResourceCopy
 ResourceCopy = copy_desc ResourceLoaded
 ps-t9 = ref ResourceCopy
endif
$phase = $phase + 1
)ini");
    f.present();f.variable("range_format",28);
    check(f.srv(4).image_view->format()==VK_FORMAT_R8G8B8A8_UNORM && f.srv(4).image==f.srv(0).image,"Fresh plain image ranges inherit the native typed format");
    for(UINT layer=0;layer<c.layers;++layer)for(UINT mip=0;mip<c.mips;++mip){auto expected=levels[layer*c.mips+mip];
        check(f.read(1,mip,layer)==expected,"Mutable image copy keeps pre-clear file bytes");
        if(!mip){const uint8_t pixel[]={51,102,153,255};for(size_t i=0;i<expected.size();++i)expected[i]=pixel[i%4];}
        check(f.read(0,mip,layer)==expected,"Original UAV clear updates mip zero and preserves higher mips");}
    f.present(2);
    check(f.srv(0).image==f.srv(2).image && f.srv(1).image==f.srv(3).image && f.srv(1).image!=f.srv(2).image,
        "Image copy cache remains independently owned after destination becomes an alias");
    check(f.srv(6).image_view==f.srv(4).image_view && f.srv(7).image_view==f.srv(5).image_view,"Fetched image ranges retain compatible source views");
    check(!f.srv(8) && f.srv(9).image==f.srv(1).image,"Null hides but retains the image copy cache; unless_null preserves null");
    check(f.read(9)==levels[0] && f.read(3)==levels[0],"Restored image copy_desc cache preserves its earlier bytes");f.release();
}
static void image_ini_depth_dds(Device& d,const std::filesystem::path& root){
    for(auto format:{DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R32G8X24_TYPELESS}){
        const bool wide=format==DXGI_FORMAT_R32G8X24_TYPELESS;const auto directory=root/(wide?"wide":"packed");
        const ImageDdsCase c{"depth",format,3,3,2,1,1,2,wide?8u:4u,1};
        const auto levels=write_image_dds(directory/"Mods/image.dds",c);
        ImageIniFixture f(d,directory,R"ini(namespace = images
[ResourceLoaded]
filename = image.dds
bind_flags = shader_resource depth_stencil
[ResourceCopy]
bind_flags = shader_resource
[Present]
ResourceCopy = copy ResourceLoaded
ps-t0 = ref ResourceLoaded
ps-t1 = ref ResourceCopy
)ini");f.present();
        for(UINT layer=0;layer<2;++layer){const auto depth=f.read(0,0,layer,1,VK_IMAGE_ASPECT_DEPTH_BIT);
            const auto stencil=f.read(0,0,layer,1,VK_IMAGE_ASPECT_STENCIL_BIT);check(depth.size()==24 && stencil.size()==6,"Packed DDS depth and stencil get independent Vulkan footprints");
            for(size_t i=0;i<6;++i)check(!memcmp(depth.data()+i*4,levels[layer].data()+i*c.bytes,wide?4:3) &&
                stencil[i]==levels[layer][i*c.bytes+(wide?4:3)],"Packed DDS depth/stencil bits survive format-specific plane extraction");
            check(f.read(1,0,layer,1,VK_IMAGE_ASPECT_DEPTH_BIT)==depth && f.read(1,0,layer,1,VK_IMAGE_ASPECT_STENCIL_BIT)==stencil,"Original copy preserves extracted depth/stencil DDS planes");}
        f.release();
    }
}
static void image_ini_failures(Device& d,const std::filesystem::path& root){
    const auto typed=image_declaration("ResourceSource","R8G8B8A8_UNORM");
    const auto typeless=image_declaration("ResourceSource","R8G8B8A8_TYPELESS");
    auto change=[](std::string text,const char* from,const char* to){const auto position=text.find(from);check(position!=std::string::npos,"Negative fixture description field exists");text.replace(position,strlen(from),to);return text;};
    const std::pair<std::string,std::string> cases[]={
        {typed+"[ResourceCopy]\nformat = R32_FLOAT\nbind_flags = shader_resource\n[Present]\nResourceCopy = copy ResourceSource\n","different DXGI families"},
        {typed+"[Constants]\nglobal $out = 0\n[Present]\nstore = $out, ResourceSource, 0\n","store requires a buffer"},
    };
    // D-150 (the original as the judge: an array-less Texture2D gives -2 queries in XXMI): a description
    // D3D11 does not create (no array slice, no samples, a quality without a pattern, a width multiplied
    // out of range, a volume with an array) leaves the resource null and a clear of it does nothing.
    for(const auto& text:{change(typed,"array = 1","array = 0"),change(typed,"msaa = 1","msaa = 0"),
                          change(typed,"msaa = 1","msaa = 1\nmsaa_quality = 1"),change(typed,"width = 2","width = 2\nwidth_multiply = -1"),
                          change(typed,"width = 2","width = 2\nwidth_multiply = 1e38"),change(typed,"type = Texture2D","type = Texture3D\ndepth = 2")}){
        static int index=0;const auto directory=root/("not-created-"+std::to_string(index++));
        ImageIniFixture f(d,directory,"namespace = images\n[Constants]\nglobal $after = 0\n"+text+"[Present]\nclear = ResourceSource 1\nps-t0 = ResourceSource\n$after = 1\n");
        f.present();f.variable("after",1);
        check(!f.srv(0).image,"A resource D3D11 does not create is null: its clear does nothing, its binding binds nothing");
        f.release();
    }
    // D-145: a view D3D11 does not make (a typed texture under another format, a typeless one with none;
    // image_source_reference checks this on WARP) leaves the slot unbound, and the Present goes on.
    for(const auto& text:{typed+"[ResourceAlias]\nformat = R8G8B8A8_UNORM_SRGB\n[Present]\nResourceAlias = ref ResourceSource\nps-t0 = ref ResourceAlias\n",
                          typeless+"[ResourceAlias]\nformat = R32_FLOAT\n[Present]\nResourceAlias = ref ResourceSource\nps-t0 = ref ResourceAlias\n",
                          typeless+"[PoolImages]\npool_size = 1\n[Present]\nPoolImages[0] = ref ResourceSource\nps-t[0:0] = ref PoolImages[0:0]\n"}){
        static int index=0;const auto directory=root/("no-view-"+std::to_string(index++));
        ImageIniFixture f(d,directory,"namespace = images\n[Constants]\nglobal $after = 0\n"+std::string(text)+"$after = 1\n");
        f.present();f.variable("after",1);
        check(!f.srv(0).image && !f.srv(0).image_view,"A view D3D11 does not make binds nothing and the Present goes on");
        f.release();
    }
    // A file the original's loader makes no texture of (not there, a truncated DDS) leaves the resource
    // null, with its warning, and the list goes on (CustomResource::LoadFromFile, D-139).
    for(const char* file:{"not-loaded.png","truncated.dds"}){const auto directory=root/file;
        if(std::string(file)=="truncated.dds"){write_image_dds(directory/"Mods/truncated.dds",{"truncated",DXGI_FORMAT_R8G8B8A8_UNORM,3,2,2,1,1,1,4,1});
            std::filesystem::resize_file(directory/"Mods/truncated.dds",148);}
        ImageIniFixture f(d,directory,"namespace = images\n[Constants]\nglobal $after = 0\n[ResourceSource]\nfilename = "+std::string(file)+
                                      "\n[Present]\nps-t0 = ref ResourceSource\n$after = 1\n");
        f.present();f.variable("after",1);
        check(!f.srv(0).image,"An image file that does not load binds nothing and the Present goes on");
        f.release();
    }
    for(size_t i=0;i<std::size(cases);++i){const auto directory=root/std::to_string(i);
        ImageIniFixture f(d,directory,"namespace = images\n"+cases[i].first);
        f.backend->begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
        declined(*f.ini,*f.backend,f.ini->present({},1,0,f.why),cases[i].second,"An invalid original image operation does nothing and is said");f.release();
    }
}
static void image_ini_pool_lifetime(Device& d,const std::filesystem::path& root){
    const auto small_data=write_image_dds(root/"Mods/small.dds",{"small",DXGI_FORMAT_R8G8B8A8_UNORM,3,2,2,1,1,1,4,1});
    const auto large_data=write_image_dds(root/"Mods/large.dds",{"large",DXGI_FORMAT_R8G8B8A8_UNORM,3,4,2,1,1,1,4,1});
    ImageIniFixture f(d,root,R"ini(namespace = images
[Constants]
global $phase = 0
[ResourceSmall]
filename = small.dds
bind_flags = shader_resource unordered_access
[ResourceLarge]
filename = large.dds
bind_flags = shader_resource unordered_access
[ResourceCopy]
bind_flags = shader_resource unordered_access
[ResourceLimited]
bind_flags = shader_resource
max_copies_per_frame = 1
[PoolImages]
pool_size = 2
bind_flags = shader_resource unordered_access
[Present]
if $phase == 0
 ResourceCopy = copy ResourceSmall
 ps-t0 = ref ResourceCopy
 ResourceCopy = copy ResourceLarge
 ps-t1 = ref ResourceCopy
 ResourceCopy = copy_desc ResourceSmall
 ps-t2 = ref ResourceCopy
 ResourceLimited = copy ResourceSmall
 ResourceLimited = copy ResourceLarge
 ps-t3 = ref ResourceLimited
 PoolImages[*] = copy ResourceSmall
 clear = PoolImages[1] 0x1 0x2 0x3 0x4
 ps-t4 = ref PoolImages[0]
 ps-t5 = ref PoolImages[1]
else
 ResourceLimited = copy ResourceLarge
 ps-t6 = ref ResourceLimited
endif
$phase = $phase + 1
)ini");f.present();
    check(f.srv(0).image==f.srv(2).image && f.srv(0).image!=f.srv(1).image,"Image copy pool keys retain different allocation descriptions");
    check(f.read(2)==small_data[0] && f.read(1)==large_data[0],"Returning to a previous image description restores its previous bytes");
    check(f.read(3)==small_data[0],"Original image per-frame copy limit preserves its first copy");
    check(f.srv(4).image!=f.srv(5).image && f.read(4)==small_data[0],"Whole-pool image copy creates independent writable allocations");
    const auto bytes=f.read(5);bool equal=bytes.size()==16;for(size_t i=0;i<bytes.size();++i)equal=equal && bytes[i]==i%4+1;
    check(equal,"Clearing one copied image pool element preserves every sibling");f.present(2);check(f.read(6)==large_data[0],"Image copy counter resets on the next original frame");
    auto held=f.srv(0).image_view;std::weak_ptr<gpu::ImageResource> weak=held->owner();f.ini.reset();f.backend.reset();
    check(!weak.expired() && held->view()!=VK_NULL_HANDLE && f.budgets.resident->used()>0,"A retained original image binding keeps its Vulkan allocation and view alive after generation retirement");
    held.reset();check(weak.expired() && !f.budgets.resident->used() && !f.budgets.staging->used(),"Last original image view releases its allocation budget");
}
