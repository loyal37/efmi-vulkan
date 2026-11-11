// CPU execution against parsed command trees, with no D3D or Vulkan device.
#include "draw_plan.h"
#include "layer_gap.h"
#include "HackerContext.h"
#include "globals.h"
#include <cstdio>

void HostParseExcerpt(const wchar_t*,const wchar_t*);
void HostParseConstants();
void HostEnumerateLists();
void HostParseResources();
void HostCompileNamedList(const wchar_t*);

using namespace efmi::frontend;

namespace {
// What the layer said it did not do as the original does, while a test runs.
std::vector<std::wstring> g_notices;
void keep_notice(const wchar_t* text){g_notices.emplace_back(text);}
bool noticed(const wchar_t* part){
    for(const auto& text:g_notices)if(text.find(part)!=std::wstring::npos)return true;
    return false;
}
}

int HostPlanTests()
{
    int failed=0,passed=0;
    auto check=[&](bool ok,const char* text){
        if(ok){++passed;printf("PASS: %s\n",text);}
        else{++failed;printf("FAIL: %s\n",text);}
    };
    registered_command_lists.clear();
    HostParseExcerpt(L"namespace = planner\n[Constants]\nglobal $choice = 0\nglobal $after = 0\n"
                     L"[ResourceIndex]\ntype = Buffer\nformat = R32_UINT\ndata = 0 1 2 2 3 0\n"
                     L"[ResourceVertices]\ntype = Buffer\nstride = 16\n"
                     L"[ResourceImage]\nfilename = fixture.dds\n[ResourceUnset]\n[CommandListNative]\n"
                     L"[CommandListDraw]\nhandling = skip\nrun = CommandListNative\n"
                     L"ib = ResourceIndex\nvb5 = ResourceVertices\n"
                     L"ps-t15 = ResourceImage\nps-t16 = null\n"
                     L"vb5->ElementFormat(BLENDINDICES, $choice) = R16G16B16A16_UINT\n"
                     L"if $choice == 0\ndrawindexed = 3, 0, -2\n"
                     L"else\ndrawindexedinstanced = 6, 2, 3, -1, 4\nendif\n"
                     L"[CommandListFail]\ndrawindexed = 3, 0, 0\ndispatch = 1, 1, 1\n"
                     L"[CommandListEmpty]\nhandling = skip\n"
                     L"[CommandListUnset]\nps-t70 = ref ResourceUnset\n"
                     L"[CommandListNegative]\ndrawindexed = -3, 0, 0\n$after = 1\n"
                     L"[CommandListNonFinite]\ndrawindexed = 1 / 0, 0, 0\n$after = 2\n"
                     L"[CommandListGPUQuery]\ndrawindexed = rt_width, 0, 0\n$after = 3\n"
                     L"[CommandListStateQuery]\n$choice = rt_width\n$after = 4\n"
                     L"[CommandListSlotQuery]\nps-t[rt_width] = ResourceImage\n$after = 5\n"
                     L"[CommandListRecursion]\nrun = CommandListRecursion\n$after = 6\n"
                     L"[CommandListVertexCopy]\nib = ResourceIndex\nvb0 = ref ResourceVertices\nvb3 = vb0\nvb0 = null\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListApplicationVertex]\nvb3 = vb1\ndrawindexed = 3, 0, 0\n$after = 7\n"
                     L"[ResourceSaved]\n"
                     L"[CommandListRestoredVs]\nResourceSaved = ref vs-t0\nvs-t0 = null\nvs-t0 = ref ResourceSaved\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListNulledVs]\nvs-t0 = null\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListMovedVs]\nResourceSaved = ref vs-t0\nvs-t1 = ref ResourceSaved\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListLeftCb]\nhandling = skip\nvs-cb2 = null\n"
                     // The draw state (D-161): every assignment is total, a draw sees the state as it stands.
                     L"[CommandListCopySlot]\nps-t0 = copy ResourceImage\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListUnlessNull]\nps-t0 = ResourceImage\nps-t0 = unless_null ResourceUnset\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListSlotToSlot]\nps-t0 = ResourceImage\nps-t1 = ref ps-t0\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListSavedVertex]\nResourceSaved = ref vb0\nvb0 = ResourceVertices\nvb0 = ref ResourceSaved\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListSavedPixel]\nResourceSaved = ps-t4\nps-t4 = ResourceImage\ndrawindexed = 3, 0, 0\nps-t4 = ResourceSaved\ndrawindexed = 6, 0, 0\n"
                     L"[CommandListCopied]\nResourceSaved = copy ResourceImage\nps-t2 = ResourceSaved\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListTargets]\nhandling = skip\no0 = null\ndrawindexed = 3, 0, 0\no0 = ref ResourceUnset\n$after = 8\n"
                     L"[CommandListTargetBack]\nhandling = skip\nResourceSaved = ref o0\no0 = null\ndrawindexed = 3, 0, 0\no0 = ref ResourceSaved\ndrawindexed = 6, 0, 0\n"
                     L"[CommandListTargetKept]\no1 = null\n"
                     L"[CommandListCompute]\ncs-t0 = ResourceImage\ncs-t1 = ResourceVertices\ncs-u2 = null\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListLater]\nhandling = skip\nps-t0 = ResourceImage\ndrawindexed = 3, 0, 0\nps-t0 = null\nvb9 = ResourceVertices\n"
                     L"[CommandListMovedPixel]\nps-t3 = ref ps-t2\ndrawindexed = 3, 0, 0\n"
                     L"[CommandListPostSkip]\npost handling = skip\n",
                     L"planner.ini");
    std::wstring budget=L"namespace = planner\n[CommandListBudget]\n";
    for(int i=0;i<257;++i)budget+=L"drawindexed = 3, 0, 0\n";
    HostParseExcerpt(budget.c_str(),L"planner.ini");
    HostEnumerateLists();HostParseConstants();HostParseResources();
    DrawPlanAdapter adapter;
    adapter.external(explicitCommandListSections.at(L"commandlist\\planner\\native"),L"test/native");
    const wchar_t* names[]={L"draw",L"fail",L"empty",L"unset",L"negative",L"nonfinite",L"recursion",L"gpuquery",L"statequery",L"slotquery",L"budget",
                            L"vertexcopy",L"applicationvertex",L"restoredvs",L"nulledvs",L"movedvs",L"leftcb",
                            L"copyslot",L"unlessnull",L"slottoslot",L"savedvertex",L"savedpixel",L"copied",L"targets",L"targetback",L"targetkept",
                            L"compute",L"later",L"movedpixel",L"postskip"};
    for(auto* name:names)HostCompileNamedList((L"commandlist\\planner\\"+std::wstring(name)).c_str());
    optimise_command_lists(nullptr);
    for(auto* name:names){
        auto& section=explicitCommandListSections.at(L"commandlist\\planner\\"+std::wstring(name));
        adapter.adapt(section.command_list);adapter.adapt(section.post_command_list);
    }
    g_notices.clear();g_layer_gap_sink=keep_notice;
    auto run=[&](const wchar_t* name,DrawPlan& plan,std::wstring& reason){
        HackerContext context;CommandListState state;DrawCallInfo caller;
        state.mHackerContext=&context;state.call_info=&caller;
        auto& list=explicitCommandListSections.at(L"commandlist\\planner\\"+std::wstring(name)).command_list;
        const bool ok=adapter.execute(list,state,plan,reason);
        check(!caller.skip && state.input_layout_overrides.empty(),"planning leaves caller skip/layout state unchanged");
        return ok;
    };
    auto& after=command_list_globals.at(L"$\\planner\\after").fval;
    auto draws=[](const DrawPlan& p){size_t n=0;for(const auto& item:p.items)n+=item.operation==PlanOperation::DrawIndexed;return n;};
    DrawPlan plan;std::wstring reason;
    check(run(L"draw",plan,reason),"upstream conditional tree emits a CPU draw plan");
    check(plan.skip_original && plan.items.size()==6 && plan.items[0].operation==PlanOperation::External &&
          plan.items[0].external==L"test/native" && plan.items[0].phase_mask!=0,
          "native boundary is explicit and survives optimisation");
    check(plan.items.size()==6 && plan.items[1].operation==PlanOperation::BindIndex &&
          plan.items[2].operation==PlanOperation::BindVertex && plan.items[2].slot==5 &&
          plan.items[3].resource && !plan.items[4].resource && plan.items[4].slot==16,
          "the bindings a draw sees stand before it, in the order the list first set them");
    check(!plan.items.empty() && plan.items.back().index_count==3 && plan.items.back().vertex_offset==-2 &&
          plan.items.back().layout.size()==1 && plan.items.back().layout[0].match.semantic_index==0,
          "indexed arguments and evaluated layout are copied into the plan");
    command_list_globals.at(L"$\\planner\\choice").fval=1;
    DrawPlan second;
    check(run(L"draw",second,reason) && second.items.back().index_count==6 && second.items.back().first_index==3 &&
          second.items.back().vertex_offset==-1 && second.items.back().instance_count==2 &&
          second.items.back().first_instance==4 && second.items.back().layout[0].match.semantic_index==1,
          "same compiled list reevaluates branches and instanced arguments");
    check(plan.items.back().layout[0].match.semantic_index==0,"later layout execution cannot mutate the earlier plan");
    const size_t retained=plan.items.size();
    // D-146: a dispatch in a replaced draw does nothing (the original's warning) and its draw goes on.
    DrawPlan dispatched;
    check(run(L"fail",dispatched,reason) && dispatched.items.size()==1 && dispatched.items[0].operation==PlanOperation::DrawIndexed,
          "a dispatch in a replaced draw does nothing and the list's draw goes on");
    // D-162: a command that cannot act does nothing, the notice names it, and its list runs to its end. Each
    // of these refused the whole execution before.
    struct Line {const wchar_t* name;float mark;const wchar_t* notice;const char* text;};
    for(const Line& line:{Line{L"negative",1,L"drawindexed = -3",   "a draw with a negative count does nothing and the list goes on"},
                          Line{L"nonfinite",2,L"drawindexed = 1 / 0","a draw with a count that is no number does nothing and the list goes on"},
                          Line{L"gpuquery",3,L"drawindexed = rt_width","a draw whose count is an operand nothing answers does nothing and the list goes on"}}){
        DrawPlan out;after=0;
        check(run(line.name,out,reason) && out.items.empty() && after==line.mark && noticed(line.notice),line.text);
    }
    {
        // D-163: an operand nothing answers reads the table's value (rt_width: the original's -1 with no
        // target bound), said once; the assignment runs.
        DrawPlan out;after=0;command_list_globals.at(L"$\\planner\\choice").fval=7;
        check(run(L"statequery",out,reason) && command_list_globals.at(L"$\\planner\\choice").fval==-1 && after==4 && noticed(L"rt_width"),
              "an operand nothing answers reads the table's value with a notice, and the assignment runs");
        command_list_globals.at(L"$\\planner\\choice").fval=0;
        // The slot -1 is outside D3D11's slots: the original's ResolveSlot warns and the binding does nothing.
        after=0;
        check(run(L"slotquery",out,reason) && out.items.empty() && after==5,"a binding at a slot number outside the slots does nothing, as the original's");
        // A list calling itself: the original stops it at its recursion limit (256 lists deep) and the caller
        // goes on. The planner's own stack bound (64 commands deep) comes first; that is the host's, so the
        // draw stays the game's, and the lists still run to their end (every level's line after the call ran).
        after=0;
        check(!run(L"recursion",out,reason) && reason.find(L"stack budget")!=std::wstring::npos && after==6,
              "a list calling itself meets the planner's stack bound: the draw stays the game's, the lists run to their end");
        // The planner's own draw budget: the draw past it is left out, alone.
        check(run(L"budget",out,reason) && out.items.size()==256 && noticed(L"Plan draw budget exceeded"),"the draw past the planner's budget is left out alone");
    }
    // The table (draw_state.h): the application's vertex buffer of another slot is no buffer the layer binds,
    // and the Mod's geometry cannot be drawn with the game's in its place: the draw stays the game's. The
    // list still runs to its end (D-162).
    after=0;
    check(!run(L"applicationvertex",plan,reason) && reason.find(L"vb3")!=std::wstring::npos && after==7 && plan.items.size()==retained &&
          plan.items.back().vertex_offset==-2,
          "the application's vertex buffer at another slot leaves the draw to the game, after the list ran to its end; the output is kept");
    // D-152, D-161: the application's binding at another stage's slot saved, nulled and put back leaves the
    // draw as the application bound it. Left changed at a draw it is degraded: the game's binding stays, the
    // notice names the line, the draw is planned (it refused the whole draw before). Left changed after the
    // last draw it reaches no draw and nothing is said.
    DrawPlan restored;g_notices.clear();
    check(run(L"restoredvs",restored,reason) && restored.items.size()==1 && restored.items[0].operation==PlanOperation::DrawIndexed && g_notices.empty(),
          "vs-t0 saved, nulled and put back plans the draw with the application's binding, and nothing is said");
    {
        DrawPlan changed;g_notices.clear();
        check(run(L"nulledvs",changed,reason) && changed.items.size()==1 && changed.items[0].operation==PlanOperation::DrawIndexed && noticed(L"vs-t0"),
              "vs-t0 nulled at a draw: the draw is planned, the game's binding stays, the notice names the slot");
        g_notices.clear();
        check(run(L"movedvs",changed,reason) && changed.items.size()==1 && noticed(L"vs-t1"),
              "the application's vs-t0 bound at vs-t1: the draw is planned, the game's binding stays, the notice names the slot");
        g_notices.clear();
        check(run(L"leftcb",changed,reason) && changed.skip_original && changed.items.empty() && g_notices.empty(),
              "a slot left changed where no draw sees it changes nothing and says nothing");
    }
    DrawPlan copied;
    check(run(L"vertexcopy",copied,reason) && copied.items.size()==4 && copied.items[0].operation==PlanOperation::BindIndex &&
          copied.items[1].operation==PlanOperation::BindVertex && copied.items[1].slot==0 && !copied.items[1].resource &&
          copied.items[2].operation==PlanOperation::BindVertex && copied.items[2].slot==3 && copied.items[2].resource &&
          copied.items[3].operation==PlanOperation::DrawIndexed,
          "vb3 = vb0 binds the buffer bound at vb0 by reference; a later change of vb0 leaves vb3; the draw sees the state as it stands");
    {
        // The draw state's assignments (D-161), each refused before.
        DrawPlan out;auto* image=&customResources.at(L"resource\\planner\\image");
        check(run(L"copyslot",out,reason) && out.items.size()==2 && out.items[0].operation==PlanOperation::BindPixelResource && out.items[0].resource==image,
              "a copy of a file of the Mod into a slot binds what the file shows");
        check(run(L"unlessnull",out,reason) && out.items.size()==2 && out.items[0].resource==image,
              "unless_null with a null source leaves the slot as it is");
        check(run(L"slottoslot",out,reason) && out.items.size()==3 && out.items[0].slot==0 && out.items[1].slot==1 && out.items[1].resource==image,
              "one pixel slot bound from another takes what that slot holds");
        check(run(L"savedvertex",out,reason) && out.items.size()==1 && out.items[0].operation==PlanOperation::DrawIndexed,
              "the game's vertex buffer held by a declaration and put back is the game's buffer in its place: nothing to bind");
        check(run(L"savedpixel",out,reason) && out.items.size()==4 && out.items[0].resource==image && out.items[1].operation==PlanOperation::DrawIndexed &&
              out.items[2].operation==PlanOperation::BindPixelResource && out.items[2].slot==4 && !out.items[2].resource && out.items[2].application_slot==4 &&
              out.items[3].index_count==6,
              "a copy of the game's texture holds it for the draw's lists: bound back, the second draw has the game's texture again");
        check(run(L"copied",out,reason) && out.items.size()==2 && out.items[0].slot==2 && out.items[0].resource==image,
              "a copy of a file of the Mod into a declaration without keys of its own shows what the file shows");
        // Changed targets: that draw alone is left out (the table's not-drawable), the list goes on.
        g_notices.clear();after=0;
        check(run(L"targets",out,reason) && out.skip_original && out.items.empty() && after==8 && noticed(L"o0"),
              "a draw with the target unbound is left out alone, with a notice, and the list goes on");
        check(run(L"targetback",out,reason) && out.skip_original && draws(out)==1 && out.items.back().index_count==6,
              "the target saved, unbound and put back: the draw between is left out, the draw after is planned");
        g_notices.clear();
        check(run(L"targetkept",out,reason) && out.skip_original && out.items.empty() && noticed(L"o1"),
              "the application's draw with a target the pre list changed is hidden, as it would not reach it");
        g_notices.clear();
        check(run(L"compute",out,reason) && out.items.size()==1 && out.items[0].operation==PlanOperation::DrawIndexed && g_notices.empty(),
              "compute slots are nothing to a draw: no item, no notice");
        check(run(L"later",out,reason) && out.skip_original && out.items.size()==2 && out.items[0].resource==image && out.items[1].operation==PlanOperation::DrawIndexed,
              "what the list binds after its last draw reaches no draw: no item");
        g_notices.clear();
        check(run(L"movedpixel",out,reason) && out.items.size()==1 && out.items[0].operation==PlanOperation::DrawIndexed && noticed(L"ps-t3"),
              "the application's texture of another pixel slot: the game's texture stays at the slot, the notice names it");
    }
    {
        // `handling = skip` after the application's draw comes too late, as in the original.
        HackerContext context;CommandListState state;DrawCallInfo caller;state.mHackerContext=&context;state.call_info=&caller;
        auto& section=explicitCommandListSections.at(L"commandlist\\planner\\postskip");
        DrawPlan out;
        check(adapter.execute_pair(section.command_list,section.post_command_list,state,out,reason) && !out.skip_original,
              "handling = skip in a post list does not hide the draw that already ran");
    }
    check(run(L"empty",plan,reason) && plan.skip_original && plan.items.empty(),"empty hidden component is a deliberate skip");
    check(run(L"unset",plan,reason) && plan.items.size()==1 && !plan.items[0].resource,
          "unassigned empty resource declaration resolves to null without a file adapter");
    CommandList alias;
    alias.SetSourceCommandList(&explicitCommandListSections.at(L"commandlist\\planner\\empty").command_list);
    HackerContext context;CommandListState state;state.mHackerContext=&context;
    check(adapter.execute(alias,state,plan,reason) && plan.skip_original && plan.items.empty(),
          "root aliases resolve to an already adapted command list");
    CommandList unadapted;
    alias.SetSourceCommandList(&unadapted);
    check(!adapter.execute(alias,state,plan,reason) && !reason.empty(),"aliases cannot enter unadapted commands");
    g_layer_gap_sink=nullptr;
    printf("XXMI draw planner: %d checks passed, %d failed\n",passed,failed);
    return failed;
}
