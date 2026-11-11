// Expose the original parser's CPU-only entry points for both hosts and the DLL.
#include <array>
#include <functional>
#include "IniHandler.cpp"

void HostParseExcerpt(const wchar_t* text,const wchar_t* ns)
{
    std::wistringstream stream(text);
    std::wstring name=ns?ns:L"";
    ParseIniStream(&stream,ns?&name:nullptr);
}
// A file by the original's own reader (ParseNamespacedIniFile): UTF-8 with a byte order mark consumed,
// text mode, and nothing of the file after a byte sequence that is not UTF-8.
void HostParseFile(const wchar_t* path,const wchar_t* ns)
{
    std::wstring name=ns?ns:L"";
    ParseNamespacedIniFile(path,ns?&name:nullptr);
}
// A file's lines as that reader gives them, for a caller that keeps only some of its sections.
std::vector<std::wstring> HostReadLines(const wchar_t* path)
{
    std::vector<std::wstring> lines;
    std::wifstream f(path,std::ios::in,_SH_DENYNO);
    if(!f)return lines;
    f.imbue(std::locale(f.getloc(),new std::codecvt_utf8<wchar_t,0x10ffff,std::consume_header>));
    for(std::wstring line;std::getline(f,line);)lines.push_back(line);
    return lines;
}
void HostCompileList(const wchar_t* name,CommandList* pre,CommandList* post)
{
    ParseCommandList(name,pre,post,nullptr,true);
}
// The command lists of a ShaderRegex group's main section as the original parses them
// (parse_shader_regex_section_main): its other keys (shader_model, temps, filter_index) are not commands.
// False when the generation has no such section; `name` is matched as the original matches section names.
bool HostCompileShaderRegexList(const wchar_t* name,CommandList* pre,CommandList* post)
{
    const auto section=ini_sections.find(name);
    if(section==ini_sections.end())return false;
    ParseCommandList(section->first.c_str(),pre,post,ShaderRegexIniKeys);
    return true;
}
void HostParseConstants() { ParseConstantsSection(); }
void HostEnumerateLists() { EnumerateExplicitCommandListSections(); }
void HostParseResources() { ParseResourceSections(); }
void HostParseIncludes() { ParseIncludedIniFiles(); }
void HostInsertBuiltins() { InsertBuiltInIniSections(); }
void HostParseShaderRegex() {
    G->assemble_signature_comments=GetIniBool(L"Rendering",L"assemble_signature_comments",false,nullptr);
    G->disassemble_undecipherable_custom_data=GetIniBool(L"Rendering",L"disassemble_undecipherable_custom_data",false,nullptr);
    G->patch_cb_offsets=GetIniBool(L"Rendering",L"patch_assembly_cb_offsets",false,nullptr);
    ParseShaderRegexSections();
}
void HostParseResourceFamily(const std::wstring& prefix)
{
    for (const auto& section : ini_sections)
        if (_wcsnicmp(section.first.c_str(), prefix.c_str(), prefix.size()) == 0)
            ParseResourceSection(section.first.c_str(), nullptr);
}

// Parse every declaration and command with the original engine. Renderer state
// is parsed as data; HLSL compilation and executing graphics are separate tests.
void HostInspectAllSections()
{
    EnumerateCustomShaderSections();
    EnumerateExplicitCommandListSections();
    EnumeratePresetOverrideSections();
    ParseConstantsSection();
    ParseResourceSections();
    ParseCommandList(L"Constants", &G->constants_command_list, &G->post_constants_command_list, nullptr);
    RegisterPresetKeyBindings();
    ParsePresetOverrideSections();
    for (auto& pair : customShaders) {
        auto* shader = &pair.second;
        const auto* name = pair.first.c_str();
        wchar_t flags[MAX_PATH];
        if (GetIniStringAndLog(name, L"flags", nullptr, flags, MAX_PATH))
            shader->compile_flags = parse_enum_option_string<const wchar_t*, D3DCompileFlags, wchar_t*>(D3DCompileFlagNames, flags, nullptr);
        ParseBlendState(shader, name); ParseDepthStencilState(shader, name);
        ParseRSState(shader, name); ParseTopology(shader, name); ParseSamplerState(shader, name);
        shader->max_executions_per_frame = GetIniInt(name, L"max_executions_per_frame", 0, nullptr);
        ParseCommandList(name, &shader->command_list, &shader->post_command_list, CustomShaderIniKeys);
    }
    ParseExplicitCommandListSections();
    ParseShaderOverrideSections();
    HostParseShaderRegex();
    // Inspection validates resizing metadata too, without creating a resource.
    G->allow_buffer_resize = true;
    ParseTextureOverrideSections();
    ParseCommandList(L"Present", &G->present_command_list, &G->post_present_command_list, nullptr);
    ParseCommandList(L"ClearRenderTargetView", &G->clear_rtv_command_list, &G->post_clear_rtv_command_list, nullptr);
    ParseCommandList(L"ClearDepthStencilView", &G->clear_dsv_command_list, &G->post_clear_dsv_command_list, nullptr);
    ParseCommandList(L"ClearUnorderedAccessViewUint", &G->clear_uav_uint_command_list, &G->post_clear_uav_uint_command_list, nullptr);
    ParseCommandList(L"ClearUnorderedAccessViewFloat", &G->clear_uav_float_command_list, &G->post_clear_uav_float_command_list, nullptr);
    size_t overrides = 0, patterns = 0, commands = 0;
    for (const auto& hashed : G->mTextureOverrideMap) overrides += hashed.second.size();
    for (const auto& regex : shader_regex_groups) patterns += regex.second.patterns.size();
    for (auto* list : registered_command_lists) if (list) commands += list->commands.size();
    printf("\"sections\":%zu,\"variables\":%zu,\"persistent\":%zu,\"resources\":%zu,\"pools\":%zu,"
           "\"command_lists\":%zu,\"commands\":%zu,\"texture_overrides\":%zu,\"custom_shaders\":%zu,"
           "\"shader_regex_groups\":%zu,\"shader_regex_patterns\":%zu",
           ini_sections.size(), command_list_globals.size(), persistent_variables.size(), customResources.size(),
           customResourcePools.size(), explicitCommandListSections.size(), commands, overrides, customShaders.size(),
           shader_regex_groups.size(), patterns);
}

// Declaration inventory from the same source getters used by CustomShader.
// This never compiles, executes a command list or writes beside installed files.
void HostVisitShaderSources(const std::function<void(const std::wstring&,const std::wstring&,UINT,int,const std::array<std::wstring,6>&)>& visit)
{
    const auto recursive=GetIniBoolOrInt(L"Rendering",L"recursive_include",false,nullptr);
    for(const auto& entry:customShaders){std::wstring path;get_namespaced_section_path(entry.first.c_str(),&path);
        std::array<std::wstring,6> sources;const wchar_t* keys[]={L"vs",L"hs",L"ds",L"gs",L"ps",L"cs"};
        for(unsigned i=0;i<6;++i){wchar_t text[MAX_PATH];if(GetIniString(entry.first.c_str(),keys[i],nullptr,text,MAX_PATH))sources[i]=text;}
        visit(entry.first,path,UINT(entry.second.compile_flags),recursive,sources);
    }
}
// The folder of the INI a section was parsed from, as the original keeps it for the files the section names
// (get_namespaced_section_path: from the root, with its separator; empty for an INI at the root). False for a
// section the parser does not hold. Valid while the generation loads.
bool HostSectionFolder(const wchar_t* section, std::wstring& folder) { return get_namespaced_section_path(section, &folder); }
void HostParseTextureOverrides() { ParseTextureOverrideSections(); }
void HostParseResourceMatching() {
    G->track_region_hashes=GetIniBool(L"Rendering",L"track_region_hashes",false,nullptr);
    if(G->track_region_hashes)BuildTextureOverrideDrawMaps();
}
void HostParseShaderOverrides() { ParseShaderOverrideSections(); }
void HostCompileNamedList(const wchar_t* name)
{
    auto& list=explicitCommandListSections.at(name);
    ParseCommandList(name,&list.command_list,&list.post_command_list,nullptr,true);
}
// Compile only the original command graph reachable from a selected root.
// Native boundaries are enumerated first and never parsed as graphics commands.
static void compile_graph(std::vector<ExplicitCommandListSection*> pending,const std::vector<CommandList*>& roots,
                          const std::vector<std::wstring>& natives)
{
    std::unordered_map<ExplicitCommandListSection*,std::wstring> names;
    for(auto& pair:explicitCommandListSections)names.emplace(&pair.second,pair.first);
    std::unordered_set<ExplicitCommandListSection*> compiled;
    // Every list once (BUG-063). The original puts one nested `if` into both the pre and the post body of
    // the `if` around it and nests each `else if` in the else before it, so a walk of every body of every
    // `if` reaches the n-th branch of an else-if chain 2^n times: a key panel's 40 options never finished
    // loading, and the engine lock it held froze the game. The original itself walks its lists flat
    // (optimise_command_lists over registered_command_lists) and runs only the branch taken.
    std::unordered_set<const CommandList*> seen;
    std::function<void(CommandList&,size_t)> visit=[&](CommandList& list,size_t depth){
        if(depth>256)throw std::runtime_error("Original command graph nesting budget exceeded");
        if(!seen.insert(&list).second)return;
        for(auto& command:list.commands){
            if(auto* call=dynamic_cast<RunExplicitCommandList*>(command.get())){
                if(call->command_list_section)pending.push_back(call->command_list_section);
            }else if(auto* copy=dynamic_cast<CopyCommandListCommand*>(command.get())){
                if(copy->src)pending.push_back(copy->src);if(copy->dst)pending.push_back(copy->dst);
            }else if(auto* branch=dynamic_cast<IfCommand*>(command.get())){
                for(auto* body:{branch->true_commands_pre.get(),branch->true_commands_post.get(),
                               branch->false_commands_pre.get(),branch->false_commands_post.get()})if(body)visit(*body,depth+1);
            }
        }
    };
    for(auto* root:roots)if(root)visit(*root,0);
    while(!pending.empty()){
        auto* section=pending.back();pending.pop_back();
        if(!compiled.insert(section).second)continue;
        if(compiled.size()>4096)throw std::runtime_error("Original command graph section budget exceeded");
        const auto& name=names.at(section);
        if(std::find(natives.begin(),natives.end(),name)!=natives.end())continue;
        if (std::find(registered_command_lists.begin(), registered_command_lists.end(), &section->command_list) == registered_command_lists.end())
            HostCompileNamedList(name.c_str());
        visit(section->command_list,0);visit(section->post_command_list,0);
    }
}
void HostCompileGraph(const std::wstring& root,const std::vector<std::wstring>& natives)
{
    compile_graph({&explicitCommandListSections.at(root)},{},natives);
}
void HostCompileRoots(const std::vector<CommandList*>& roots,const std::vector<std::wstring>& natives)
{
    compile_graph({},roots,natives);
}
void HostCompileEntries(const std::vector<ExplicitCommandListSection*>& entries, const std::vector<std::wstring>& natives)
{
    compile_graph(entries, {}, natives);
}
void HostCompileRemainingLists(const std::vector<std::wstring>& natives)
{
    // Original XXMI parses dormant command lists too. Do not accept a typo
    // merely because the current character's graph does not reach it.
    for (auto& entry : explicitCommandListSections) {
        if (std::find(natives.begin(), natives.end(), entry.first) != natives.end()) continue;
        if (std::find(registered_command_lists.begin(), registered_command_lists.end(), &entry.second.command_list) != registered_command_lists.end()) continue;
        HostCompileNamedList(entry.first.c_str());
    }
}
// Callback-only offline inspection. Core pools/dynamic resources are outside
// this fixture; their existence is not implied by these file declarations.
void HostParseFileResources(size_t root_length)
{
    auto lower=ini_sections.lower_bound(L"Resource");
    auto upper=prefix_upper_bound(ini_sections,std::wstring(L"Resource"));
    for(auto it=lower;it!=upper;++it){
        wchar_t name[MAX_PATH];
        if(!GetIniString(it->first.c_str(),L"filename",nullptr,name,MAX_PATH))continue;
        std::wstring path;get_namespaced_section_path(it->first.c_str(),&path);
        if(root_length+path.size()+wcslen(name)+2>=MAX_PATH)
            throw std::runtime_error("Resource filename exceeds original parser path capacity");
        ParseResourceSection(it->first.c_str(),nullptr);
    }
}
