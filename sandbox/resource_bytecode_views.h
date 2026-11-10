#include "../frontend/gpu/dxbc_bindings.h"
struct BytecodeViewReference {char kind;unsigned slot;frontend::GpuResourceBinding binding;};
static std::vector<std::vector<uint8_t>> native_bytecode_views(const gpu::HlslProgram& program,
    const std::vector<BytecodeViewReference>& inputs,gpu::ResourceCommands& resources,VkQueue queue){
    ImageNativeReference native;
    std::vector<ImageNativeReference::Ptr<ID3D11Buffer>> buffers;
    std::vector<ImageNativeReference::Ptr<ID3D11ShaderResourceView>> srvs;
    std::vector<ImageNativeReference::Ptr<ID3D11UnorderedAccessView>> uavs;
    for(const auto& input:inputs){const auto& source=input.binding;
        const auto bytes=resources.readback(queue,source.buffer,0,source.buffer->size());
        auto description=source.description;description.Usage=D3D11_USAGE_DEFAULT;description.CPUAccessFlags=0;
        D3D11_SUBRESOURCE_DATA data{bytes.data(),0,0};ImageNativeReference::Ptr<ID3D11Buffer> buffer;
        check(SUCCEEDED(native.device->CreateBuffer(&description,&data,&buffer)),"Native INI bytecode buffer allocation");buffers.push_back(buffer);
        if(input.kind=='b'){auto* ptr=buffer.Get();native.context->CSSetConstantBuffers(input.slot,1,&ptr);continue;}
        const bool raw=source.view_format==DXGI_FORMAT_R32_TYPELESS;
        const unsigned width=raw?4:source.view_format==DXGI_FORMAT_UNKNOWN?description.StructureByteStride:dxgi_format_size(source.view_format);
        check(width && source.view_offset%width==0 && source.view_size%width==0,"Native INI bytecode view element boundaries");
        if(input.kind=='t'){D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=source.view_format;
            view.ViewDimension=raw?D3D11_SRV_DIMENSION_BUFFEREX:D3D11_SRV_DIMENSION_BUFFER;
            view.BufferEx.FirstElement=source.view_offset/width;view.BufferEx.NumElements=source.view_size/width;
            if(raw)view.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
            ImageNativeReference::Ptr<ID3D11ShaderResourceView> srv;check(SUCCEEDED(native.device->CreateShaderResourceView(buffer.Get(),&view,&srv)),"Native INI typed/raw/structured SRV");
            auto* ptr=srv.Get();native.context->CSSetShaderResources(input.slot,1,&ptr);srvs.push_back(srv);
        }else{D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=source.view_format;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
            view.Buffer.FirstElement=source.view_offset/width;view.Buffer.NumElements=source.view_size/width;if(raw)view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
            ImageNativeReference::Ptr<ID3D11UnorderedAccessView> uav;check(SUCCEEDED(native.device->CreateUnorderedAccessView(buffer.Get(),&view,&uav)),"Native INI typed/raw/structured UAV");
            auto* ptr=uav.Get();native.context->CSSetUnorderedAccessViews(input.slot,1,&ptr,nullptr);uavs.push_back(uav);}
    }
    ImageNativeReference::Ptr<ID3D11ComputeShader> shader;
    check(SUCCEEDED(native.device->CreateComputeShader(program.dxbc.data(),program.dxbc.size(),nullptr,&shader)),"Native exact bytecode view reference shader");
    native.context->CSSetShader(shader.Get(),nullptr,0);native.context->Dispatch(2,1,1);
    std::vector<std::vector<uint8_t>> result;
    for(size_t n=0;n<inputs.size();++n)if(inputs[n].kind=='u'){
        auto desc=inputs[n].binding.description;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;ImageNativeReference::Ptr<ID3D11Buffer> readback;
        check(SUCCEEDED(native.device->CreateBuffer(&desc,nullptr,&readback)),"Native INI bytecode staging buffer");native.context->CopyResource(readback.Get(),buffers[n].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(native.context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"Native INI bytecode view readback");
        result.emplace_back(static_cast<const uint8_t*>(mapped.pData),static_cast<const uint8_t*>(mapped.pData)+desc.ByteWidth);native.context->Unmap(readback.Get(),0);
    }
    return result;
}
static void bytecode_ini_views(Device& d,const std::filesystem::path& root){
    gpu::DxbcCompiler compiler(L"F:/vulkan/vkd3d-2.1-build/efmi-vkd3d-shader.dll");
    using Kind=gpu::ShaderBufferKind;
    for(unsigned mode=0;mode<4;++mode){const unsigned components=mode==0?3:4,stride=components*4;
        const bool raw=mode==2,typed=mode==3;
        std::string hlsl;
        if(typed)hlsl=R"hlsl(RWBuffer<float4> Color:register(u0);RWStructuredBuffer<uint4> Words:register(u1);
[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){float4 v=Color[id.x];Color[id.x]=float4(v.xyz*0.5,1);Words[id.x]=uint4(id.x+1,7,11,13);})hlsl";
        else{hlsl="cbuffer Parameters:register(b2){uint Add;};StructuredBuffer<uint"+std::to_string(components)+"> Input:register(t3);";
            hlsl+="ByteAddressBuffer Raw:register(t4);Buffer<uint> Typed:register(t6);";
            hlsl+=raw?"RWByteAddressBuffer Output:register(u0);":"RWStructuredBuffer<uint"+std::to_string(components)+"> Output:register(u0);";
            hlsl+="[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){uint"+std::to_string(components)+" v=Input[1-id.x]+Add+Raw.Load(id.x*4)+Typed[id.x];";
            hlsl+=raw?"Output.Store4(id.x*16,v);}":"Output[id.x]=v;}";}
        const auto bytes=bytecode_source('c',hlsl.c_str());const auto rebuilt=bytecode_roundtrip(bytes);
        const auto contract=gpu::dxbc_buffer_contracts(rebuilt);
        check(contract.at({'u',typed?1u:0u}).kind==(raw?Kind::Raw:Kind::Structured) &&
            contract.at({'u',typed?1u:0u}).stride==(raw?0:stride),"Original UAV declaration retains its source stride without RDEF");
        auto broken=rebuilt;broken.resize(31);refuses([&]{gpu::dxbc_buffer_contracts(broken);},"Truncated declaration contract refuses before reading chunks");
        auto program=translated_bytecode(compiler,rebuilt,'c',gpu::DxbcFloat32Mode::Software);
        const std::string numbers="1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24";
        const std::string zeros="0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
        std::string ini="namespace = compute\n[Constants]\n";
        if(typed)ini+="cs-u0 = ref ResourceColor->region(16, 24)\ncs-u1 = ref ResourceOutput->region(48, 80)\n";
        else ini+="cs-cb2 = ResourceParameters\ncs-t3 = ref ResourceInput->region(48, "+std::to_string(48+2*stride)+")\n"
            "cs-t4 = ref ResourceRaw->region(16, 24)\ncs-t6 = ref ResourceTyped->region(16, 24)\n"
            "cs-u0 = ref ResourceOutput->region(48, "+std::to_string(48+2*stride)+")\n";
        ini+="[Present]\ndispatch = 2, 1, 1\n[ResourceInput]\ntype = StructuredBuffer\nstride = "+std::to_string(stride)+"\nbind_flags = shader_resource\ndata = R32_UINT "+numbers+"\n"
            "[ResourceRaw]\ntype = ByteAddressBuffer\nbind_flags = shader_resource\ndata = R32_UINT 0 0 0 0 11 13 0 0\n"
            "[ResourceTyped]\ntype = Buffer\nformat = R32_UINT\nbind_flags = shader_resource\ndata = R32_UINT 0 0 0 0 17 19 0 0\n"
            "[ResourceParameters]\ntype = Buffer\nbind_flags = constant_buffer\ndata = R32_UINT 23 0 0 0\n"
            "[ResourceColor]\ntype = Buffer\nformat = R8G8B8A8_UNORM\nbind_flags = unordered_access\ndata = R32_UINT 0 0 0 0 2151686160 2151686160 0 0\n"
            "[ResourceOutput]\ntype = "+std::string(raw?"ByteAddressBuffer":"StructuredBuffer")+"\nstride = "+std::to_string(stride)+
            "\nbind_flags = unordered_access shader_resource\ndata = R32_UINT "+zeros+"\n";
        ShaderFixture f(d,root/std::to_string(mode),ini.c_str());f.backend->set_programs({program});
        auto& resources=f.backend->resources();std::vector<BytecodeViewReference> views;
        auto add=[&](char kind,unsigned slot){views.push_back({kind,slot,resources.binding(L'c',kind=='b'?ResourceCopyTargetType::CONSTANT_BUFFER:
            kind=='t'?ResourceCopyTargetType::SHADER_RESOURCE:ResourceCopyTargetType::UNORDERED_ACCESS_VIEW,slot)});};
        if(typed){add('u',0);add('u',1);}else{add('b',2);add('t',3);add('t',4);add('t',6);add('u',0);}
        const auto expected=native_bytecode_views(program,views,resources.commands(),d.queue);
        try{f.present();}catch(const std::exception& e){throw std::runtime_error("Bytecode INI case"+std::to_string(mode)+": "+e.what());}size_t output=0;
        for(const auto& view:views)if(view.kind=='u'){
            check(resources.commands().readback(d.queue,view.binding.buffer,0,view.binding.buffer->size())==expected[output++],
                "Original INI bytecode bindings preserve native view offsets, structures and typed conversions");}
        if(!typed){auto incompatible=program;for(auto& binding:incompatible.bindings)if(binding.source_type=='t' && binding.slot==3)binding.source_stride+=4;
            f.backend->set_programs({incompatible});resources.begin(d.queue,frontend::GpuResourceRuntime::ExecutionSite::OutsideRenderPass);
            declined(*f.ini,resources,f.ini->present({},++f.frame,0,f.why),"structured buffer view","A wrong native structure stride leaves its CustomShader undone");
            check(resources.commands().readback(d.queue,views.back().binding.buffer,0,views.back().binding.buffer->size())==expected[0],"Refused bytecode binding leaves previous GPU output intact");}
        views.clear();f.release();
        if(typed)for(unsigned guard=0;guard<4;++guard){auto restricted=d.resource;gpu::ResourceBudgets budgets;auto host=shader_host(d);
            if(guard==0)restricted.enabled_features.shaderStorageImageReadWithoutFormat=VK_FALSE;
            if(guard==1)restricted.enabled_features.shaderStorageImageWriteWithoutFormat=VK_FALSE;
            if(guard==2)restricted.format_features2=false;
            if(guard==3)restricted.GetFormatProperties2=[](VkPhysicalDevice,VkFormat,VkFormatProperties2* out){
                auto* extra=static_cast<VkFormatProperties3*>(out->pNext);extra->bufferFeatures=VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT;};
            {gpu::ResourceCommands resources(restricted,budgets);gpu::ShaderCommands commands(resources,host);
                if(guard<2)refuses([&]{commands.compile(program);},"Typed bytecode reads/writes require actual logical feature enablement");
                else{auto pipeline=commands.compile(program);
                    auto color=resources.create(8,VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,nullptr,0,true);
                    auto words=resources.create(32,VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,nullptr,0,true);
                    refuses([&]{commands.dispatch(pipeline,{buffer_descriptor('u',0,color,VK_FORMAT_R8G8B8A8_UNORM),
                        buffer_descriptor('u',1,words,VK_FORMAT_R32_UINT)},{2,1,1});},"Unformatted storage refuses missing query enablement or per-format capability");}
            }
            check(!budgets.resident->used() && !budgets.staging->used() && !host.objects->used(),"Refused typed-bytecode capabilities retain no GPU owners");
        }
    }
}
