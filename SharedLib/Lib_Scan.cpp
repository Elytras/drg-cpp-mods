#include "Lib_Scan.h"
#include "Lib_PropertyAccess.h"  // GetTypeName
#include "Lib_ObjectCast.h"
#include <algorithm>

using namespace SDK;

namespace Scan
{

std::string BuildExplicitCallName(UObject* Owner, UFunction* Func)
{
    const std::string ownerClass = Owner && Owner->Class ? Owner->Class->GetName() : "?";
    const std::string ownerName  = Owner ? Owner->GetName() : "?";
    const std::string funcName   = Func  ? Func ->GetName() : "?";
    return ownerClass + "::" + ownerName + "::" + funcName;
}

std::string BuildFuncSig(UFunction* Func)
{
    std::string name = Func->GetName();
    struct ParamEntry { std::string text; };
    std::vector<ParamEntry> params;
    params.reserve(8);
    size_t totalParamChars = 0;
    for (auto* field : FFieldRange(Func->ChildProperties))
    {
        if (!FieldCast::IsA<FProperty>(field)) continue;
        auto* Prop  = static_cast<FProperty*>(field);
        auto  pflags = static_cast<EPropertyFlags>(Prop->PropertyFlags);
        if (pflags & EPropertyFlags::ReturnParm) continue;
        if (!(pflags & EPropertyFlags::Parm))    continue;
        auto& e = params.emplace_back();
        e.text  = Prop->Name.ToString();
        e.text += ": ";
        e.text += GetTypeName(field);
        totalParamChars += e.text.size();
    }
    if (params.empty()) { std::string r = name; r += "()"; return r; }

    const size_t singleSize = name.size() + 1 + totalParamChars + (params.size() - 1) * 2 + 1;
    constexpr size_t LineLimit = 80;
    if (singleSize <= LineLimit)
    {
        std::string r = name; r += '(';
        for (size_t i = 0; i < params.size(); ++i) { if (i) r += ", "; r += params[i].text; }
        r += ')';
        return r;
    }
    constexpr std::string_view prefix = "║       ", close = "║   )";
    std::string r = name; r += "(\n";
    for (size_t i = 0; i < params.size(); ++i)
    {
        r += prefix; r += params[i].text;
        if (i < params.size() - 1) r += ',';
        r += '\n';
    }
    r += close;
    return r;
}

void ScanFunctions(UObject* Obj, std::vector<std::string>& out)
{
    if (!Obj || !Obj->Class) return;
    for (UClass* cls : UClassHierarchyRange(Obj->Class))
        for (auto* field : UFieldRange(cls->Children))
        {
            if (!field->IsA(UFunction::StaticClass())) continue;
            auto* Func  = static_cast<UFunction*>(field);
            auto  flags = static_cast<EFunctionFlags>(Func->FunctionFlags);
            if (!(flags & EFunctionFlags::Net))       continue;
            if (!(flags & EFunctionFlags::NetServer)) continue;
            out.push_back(BuildFuncSig(Func));
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

ScanAllResult ScanAllClasses()
{
    ScanAllResult result{};
    info("[scanall] Starting global CDO scan...");
    for (int i = 0; i < UObject::GObjects->Num(); ++i)
    {
        UObject* Obj = UObject::GObjects->GetByIndex(i);
        if (!Obj || !Obj->IsA(UClass::StaticClass())) continue;
        UClass*  Class = static_cast<UClass*>(Obj);
        UObject* CDO   = Class->ClassDefaultObject;
        if (!CDO) continue;

        std::vector<std::string> classFuncs;
        for (auto* field : UFieldRange(Class->Children))
        {
            if (!field->IsA(UFunction::StaticClass())) continue;
            auto* Func  = static_cast<UFunction*>(field);
            auto  flags = static_cast<EFunctionFlags>(Func->FunctionFlags);
            if (!(flags & EFunctionFlags::Net))       continue;
            if (!(flags & EFunctionFlags::NetServer)) continue;
            classFuncs.push_back(BuildFuncSig(Func));
        }
        if (classFuncs.empty()) continue;
        std::sort(classFuncs.begin(), classFuncs.end());
        classFuncs.erase(std::unique(classFuncs.begin(), classFuncs.end()), classFuncs.end());
        ++result.classes;
        info("╔══ Class: {}", Class->GetName());
        info("╠═ [CDO] {} server RPCs found", classFuncs.size());
        for (size_t j = 0; j < classFuncs.size(); ++j)
        {
            info("║   {} {}", j == classFuncs.size() - 1 ? "└──" : "├──", classFuncs[j]);
            ++result.functions;
        }
        info("╚══════════════════════════");
    }
    info("[scanall] Done — {} classes, {} unique server RPCs", result.classes, result.functions);
    return result;
}

} // namespace Scan
