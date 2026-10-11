#include "anduefker/ir/ReflectionLayout.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace anduefker::ir
{
    namespace
    {
        struct Interval
        {
            const PropertyIR *property = nullptr;
            int64_t end = 0;
            bool boolStorage = false;
            uint64_t mask = 0;
        };

        bool Overlaps(const PropertyIR &left, int64_t leftEnd, const PropertyIR &right, int64_t rightEnd)
        {
            return static_cast<int64_t>(left.offset) < rightEnd && static_cast<int64_t>(right.offset) < leftEnd;
        }

        void AddIssue(LayoutAnalysisIR &analysis, LayoutIssueKind kind, const PropertyIR &property,
                      const PropertyIR *conflicting, bool affectsCompleteness, std::string message)
        {
            analysis.issues.push_back({kind, property.address, conflicting ? conflicting->address : 0,
                                       affectsCompleteness, std::move(message)});
            if (affectsCompleteness || kind == LayoutIssueKind::InheritedExtentIntersection)
                analysis.representation = LayoutRepresentation::OffsetDescription;
        }

        void AnalyzeOwnerLayout(const std::vector<PropertyIR> &properties, int32_t bound, LayoutAnalysisIR &analysis)
        {
            std::vector<const PropertyIR *> ordered;
            ordered.reserve(properties.size());
            for (const auto &property : properties)
                ordered.push_back(&property);
            std::stable_sort(ordered.begin(), ordered.end(), [](const PropertyIR *left, const PropertyIR *right)
                             { return left->offset < right->offset; });

            std::vector<Interval> intervals;
            for (const PropertyIR *property : ordered)
            {
                if (!property->typeDetailsResolved)
                {
                    AddIssue(analysis, LayoutIssueKind::MissingTypeInformation, *property, nullptr, true,
                             "field type information is incomplete: name=" + property->name +
                                 " reflected_class=" + property->reflectedClass);
                }
                int64_t end = 0;
                if (!IsValidPropertyBounds(*property, bound, end))
                {
                    AddIssue(analysis, LayoutIssueKind::InvalidBounds, *property, nullptr, true,
                             "field has invalid bounds: name=" + property->name +
                                 " offset=" + std::to_string(property->offset) +
                                 " element_size=" + std::to_string(property->elementSize) +
                                 " array_dim=" + std::to_string(property->arrayDim) +
                                 " bound=" + std::to_string(bound));
                    continue;
                }

                const bool boolStorage = IsValidPropertyBoolLayout(*property);
                const uint64_t mask = boolStorage ? PropertyBoolMask(*property) : 0;
                for (const Interval &existing : intervals)
                {
                    if (!Overlaps(*property, end, *existing.property, existing.end))
                        continue;
                    const bool sharedBoolStorage = boolStorage && existing.boolStorage &&
                                                   property->offset == existing.property->offset && end == existing.end;
                    if (sharedBoolStorage && (mask & existing.mask) == 0)
                        continue;
                    const LayoutIssueKind kind = sharedBoolStorage ? LayoutIssueKind::BoolMaskConflict
                                                                   : LayoutIssueKind::SameOwnerConflict;
                    AddIssue(analysis, kind, *property, existing.property, true,
                             (kind == LayoutIssueKind::BoolMaskConflict ? "bool masks overlap" : "fields overlap") +
                                 std::string(": name=") + property->name + " conflicting=" + existing.property->name);
                }
                intervals.push_back({property, end, boolStorage, mask});
            }
        }

        bool CollectBaseChain(const std::unordered_map<uintptr_t, TypeIR *> &types, uintptr_t address,
                              std::vector<const TypeIR *> &chain)
        {
            std::unordered_set<uintptr_t> visited;
            uintptr_t current = address;
            while (current != 0)
            {
                if (!visited.insert(current).second)
                    return false;
                const auto found = types.find(current);
                if (found == types.end())
                    return false;
                chain.push_back(found->second);
                current = found->second->superAddress;
            }
            return true;
        }

        std::vector<Interval> InheritedIntervals(const std::vector<const TypeIR *> &chain)
        {
            std::vector<Interval> result;
            for (const TypeIR *base : chain)
            {
                for (const auto &property : base->properties)
                {
                    int64_t end = 0;
                    if (!IsValidPropertyBounds(property, base->size, end))
                        continue;
                    result.push_back({&property, end, IsValidPropertyBoolLayout(property),
                                      IsValidPropertyBoolLayout(property) ? PropertyBoolMask(property) : 0});
                }
            }
            return result;
        }

        void AnalyzeType(TypeIR &type, const std::unordered_map<uintptr_t, TypeIR *> &types, ReflectionStats &stats)
        {
            type.layout = {};
            type.layout.analyzed = true;
            AnalyzeOwnerLayout(type.properties, type.size, type.layout);

            if (type.superAddress != 0)
            {
                std::vector<const TypeIR *> chain;
                type.layout.typeGraphComplete = CollectBaseChain(types, type.superAddress, chain);
                if (type.layout.typeGraphComplete && !chain.empty())
                {
                    const TypeIR *base = chain.front();
                    if (base->size >= 0 && base->size <= type.size)
                    {
                        type.layout.baseExtentKnown = true;
                        type.layout.baseExtent = base->size;
                        const auto inherited = InheritedIntervals(chain);
                        for (const auto &property : type.properties)
                        {
                            int64_t end = 0;
                            if (!IsValidPropertyBounds(property, type.size, end))
                                continue;
                            for (const Interval &baseField : inherited)
                            {
                                if (property.address == baseField.property->address)
                                    continue;
                                if (!Overlaps(property, end, *baseField.property, baseField.end))
                                    continue;
                                AddIssue(type.layout, LayoutIssueKind::KnownInheritedFieldConflict, property,
                                         baseField.property, true,
                                         "field intersects known inherited field: name=" + property.name +
                                             " inherited=" + baseField.property->name);
                            }
                            if (static_cast<int64_t>(property.offset) < type.layout.baseExtent && end > 0)
                            {
                                AddIssue(type.layout, LayoutIssueKind::InheritedExtentIntersection, property, nullptr, false,
                                         "field intersects known base extent: name=" + property.name +
                                             " base_extent=" + std::to_string(type.layout.baseExtent));
                            }
                        }
                    }
                    else
                        type.layout.representation = LayoutRepresentation::OffsetDescription;
                }
                else
                    type.layout.representation = LayoutRepresentation::OffsetDescription;
            }
            for (const LayoutIssueIR &issue : type.layout.issues)
            {
                if (issue.affectsCompleteness)
                {
                    ++stats.layoutConflicts;
                    type.layoutConflicts.push_back(issue.message);
                }
            }
            if (!type.layout.issues.empty() &&
                std::any_of(type.layout.issues.begin(), type.layout.issues.end(), [](const LayoutIssueIR &issue)
                            { return issue.affectsCompleteness; }))
                type.status = ParseStatus::Partial;
        }

        void AnalyzeFunction(FunctionIR &function, ReflectionStats &stats)
        {
            function.layout = {};
            function.layout.analyzed = true;
            AnalyzeOwnerLayout(function.parameters, function.paramSize, function.layout);
            for (const LayoutIssueIR &issue : function.layout.issues)
            {
                if (issue.affectsCompleteness)
                {
                    ++stats.layoutConflicts;
                    function.layoutConflicts.push_back(issue.message);
                }
            }
            if (std::any_of(function.layout.issues.begin(), function.layout.issues.end(), [](const LayoutIssueIR &issue)
                            { return issue.affectsCompleteness; }))
                function.status = ParseStatus::Partial;
        }
    } // namespace

    void AnalyzeReflectionLayouts(ReflectionIR &reflection)
    {
        std::unordered_map<uintptr_t, TypeIR *> types;
        for (auto &type : reflection.types)
            types.emplace(type.address, &type);
        for (auto &type : reflection.types)
            AnalyzeType(type, types, reflection.stats);
        for (auto &[address, function] : reflection.functions)
        {
            (void)address;
            if (function.headerReadable)
                AnalyzeFunction(function, reflection.stats);
        }

        // A base represented only by offset descriptors cannot provide a native C++
        // extent for a sequential derived declaration. Propagate that representation.
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (auto &type : reflection.types)
            {
                const auto base = types.find(type.superAddress);
                if (base != types.end() && base->second->layout.representation == LayoutRepresentation::OffsetDescription &&
                    type.layout.representation != LayoutRepresentation::OffsetDescription)
                {
                    type.layout.representation = LayoutRepresentation::OffsetDescription;
                    changed = true;
                }
            }
        }
    }

    TypeDeclarationPlan PlanTypeDeclarations(const ReflectionIR &reflection)
    {
        TypeDeclarationPlan result;
        const size_t count = reflection.types.size();
        std::unordered_map<uintptr_t, size_t> byAddress;
        for (size_t index = 0; index < count; ++index)
        {
            const auto &type = reflection.types[index];
            byAddress.emplace(type.address, index);
            result.types.emplace(type.address, TypeDeclarationIR{type.layout.representation, false, false, type.size});
        }
        std::vector<size_t> pending(count);
        std::vector<std::vector<size_t>> dependents(count);
        std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> ready;
        for (size_t index = 0; index < count; ++index)
        {
            const auto &type = reflection.types[index];
            auto &info = result.types.at(type.address);
            const auto base = byAddress.find(type.superAddress);
            info.inheritsBase = info.representation == LayoutRepresentation::SequentialMembers &&
                                base != byAddress.end() &&
                                result.types.at(base->first).representation == LayoutRepresentation::SequentialMembers &&
                                reflection.types[base->second].size >= 0 && reflection.types[base->second].size <= type.size;
            std::unordered_set<size_t> dependencies;
            if (info.inheritsBase)
                dependencies.insert(base->second);
            if (info.representation == LayoutRepresentation::SequentialMembers)
                for (const auto &property : type.properties)
                {
                    const auto target = byAddress.find(property.type.referencedObject);
                    if (property.type.kind == PropertyKind::Struct && target != byAddress.end() &&
                        result.types.at(target->first).representation == LayoutRepresentation::SequentialMembers)
                        dependencies.insert(target->second);
                }
            pending[index] = dependencies.size();
            for (size_t dependency : dependencies)
                dependents[dependency].push_back(index);
            if (dependencies.empty())
                ready.push(index);
        }
        while (!ready.empty())
        {
            const size_t index = ready.top();
            ready.pop();
            result.order.push_back(index);
            for (size_t dependent : dependents[index])
                if (--pending[dependent] == 0)
                    ready.push(dependent);
        }
        for (size_t index = 0; index < count; ++index)
            if (pending[index] != 0)
            {
                auto &info = result.types.at(reflection.types[index].address);
                info.representation = LayoutRepresentation::OffsetDescription;
                info.dependencyBlocked = true;
                info.inheritsBase = false;
                result.order.push_back(index);
            }
        return result;
    }

    bool HasStorageRepresentationGap(const TypeReferenceIR &reference, const TypeDeclarationPlan &plan, size_t depth)
    {
        if (depth >= 32 || !reference.detailsResolved || reference.arrayDim != 1)
            return true;
        switch (reference.kind)
        {
        case PropertyKind::Struct:
        {
            const auto type = plan.types.find(reference.referencedObject);
            return type == plan.types.end() || type->second.size != reference.elementSize ||
                   type->second.representation == LayoutRepresentation::OffsetDescription;
        }
        case PropertyKind::Text:
        case PropertyKind::LazyObject:
        case PropertyKind::SoftObject:
        case PropertyKind::SoftClass:
        case PropertyKind::Delegate:
        case PropertyKind::FieldPath:
        case PropertyKind::Optional:
        case PropertyKind::Unknown:
            return true;
        case PropertyKind::WeakObject:
            return reference.elementSize != 8 || !plan.types.contains(reference.referencedObject);
        case PropertyKind::MulticastDelegate:
            return reference.delegateStorage != DelegateStorageKind::SparseMulticast || reference.elementSize != 1;
        case PropertyKind::Array:
        case PropertyKind::Map:
        case PropertyKind::Set:
            if (depth != 0)
                return true;
            [[fallthrough]];
        default:
            return (reference.inner && HasStorageRepresentationGap(*reference.inner, plan, depth + 1)) ||
                   (reference.key && HasStorageRepresentationGap(*reference.key, plan, depth + 1)) ||
                   (reference.value && HasStorageRepresentationGap(*reference.value, plan, depth + 1));
        }
    }
} // namespace anduefker::ir
