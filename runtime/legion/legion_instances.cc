/* Copyright 2020 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "legion.h"
#include "legion/runtime.h"
#include "legion/legion_ops.h"
#include "legion/legion_tasks.h"
#include "legion/region_tree.h"
#include "legion/legion_spy.h"
#include "legion/legion_context.h"
#include "legion/legion_profiling.h"
#include "legion/legion_instances.h"
#include "legion/legion_views.h"

namespace LegionRuntime {
  namespace Accessor {
    namespace DebugHooks {
      // these are calls that can be implemented by a higher level (e.g. Legion) to
      //  perform privilege/bounds checks on accessor reference and produce more useful
      //  information for debug

      /*extern*/ void (*check_bounds_ptr)(void *region, ptr_t ptr) = 0;
      /*extern*/ void (*check_bounds_dpoint)(void *region, const Legion::DomainPoint &dp) = 0;

      /*extern*/ const char *(*find_privilege_task_name)(void *region) = 0;
    };
  };
};

namespace Legion {
  namespace Internal {

    LEGION_EXTERN_LOGGER_DECLARATIONS

    // This is the shit right here: super-cool helper function

    //--------------------------------------------------------------------------
    template<unsigned LOG2MAX>
    static inline void compress_mask(FieldMask &x, FieldMask m)
    //--------------------------------------------------------------------------
    {
      FieldMask mk, mp, mv, t;
      // See hacker's delight 7-4
      x = x & m;
      mk = ~m << 1;
      for (unsigned i = 0; i < LOG2MAX; i++)
      {
        mp = mk ^ (mk << 1);
        for (unsigned idx = 1; idx < LOG2MAX; idx++)
          mp = mp ^ (mp << (1 << idx));
        mv = mp & m;
        m = (m ^ mv) | (mv >> (1 << i));
        t = x & mv;
        x = (x ^ t) | (t >> (1 << i));
        mk = mk & ~mp;
      }
    }

    /////////////////////////////////////////////////////////////
    // Copy Across Helper 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    void CopyAcrossHelper::compute_across_offsets(const FieldMask &src_mask,
                                       std::vector<CopySrcDstField> &dst_fields)
    //--------------------------------------------------------------------------
    {
      FieldMask compressed; 
      bool found_in_cache = false;
      for (LegionDeque<std::pair<FieldMask,FieldMask> >::aligned::const_iterator
            it = compressed_cache.begin(); it != compressed_cache.end(); it++)
      {
        if (it->first == src_mask)
        {
          compressed = it->second;
          found_in_cache = true;
          break;
        }
      }
      if (!found_in_cache)
      {
        compressed = src_mask;
        compress_mask<STATIC_LOG2(LEGION_MAX_FIELDS)>(compressed, full_mask);
        compressed_cache.push_back(
            std::pair<FieldMask,FieldMask>(src_mask, compressed));
      }
      int pop_count = FieldMask::pop_count(compressed);
#ifdef DEBUG_LEGION
      assert(pop_count == FieldMask::pop_count(src_mask));
#endif
      unsigned offset = dst_fields.size();
      dst_fields.resize(offset + pop_count);
      int next_start = 0;
      for (int idx = 0; idx < pop_count; idx++)
      {
        int index = compressed.find_next_set(next_start);
        CopySrcDstField &field = dst_fields[offset+idx];
        field = offsets[index];
        // We'll start looking again at the next index after this one
        next_start = index + 1;
      }
    }

    //--------------------------------------------------------------------------
    FieldMask CopyAcrossHelper::convert_src_to_dst(const FieldMask &src_mask)
    //--------------------------------------------------------------------------
    {
      FieldMask dst_mask;
      if (!src_mask)
        return dst_mask;
      if (forward_map.empty())
      {
#ifdef DEBUG_LEGION
        assert(src_indexes.size() == dst_indexes.size());
#endif
        for (unsigned idx = 0; idx < src_indexes.size(); idx++)
        {
#ifdef DEBUG_LEGION
          assert(forward_map.find(src_indexes[idx]) == forward_map.end());
#endif
          forward_map[src_indexes[idx]] = dst_indexes[idx];
        }
      }
      int index = src_mask.find_first_set();
      while (index >= 0)
      {
#ifdef DEBUG_LEGION
        assert(forward_map.find(index) != forward_map.end());
#endif
        dst_mask.set_bit(forward_map[index]);
        index = src_mask.find_next_set(index+1);
      }
      return dst_mask;
    }

    //--------------------------------------------------------------------------
    FieldMask CopyAcrossHelper::convert_dst_to_src(const FieldMask &dst_mask)
    //--------------------------------------------------------------------------
    {
      FieldMask src_mask;
      if (!dst_mask)
        return src_mask;
      if (backward_map.empty())
      {
#ifdef DEBUG_LEGION
        assert(src_indexes.size() == dst_indexes.size());
#endif
        for (unsigned idx = 0; idx < dst_indexes.size(); idx++)
        {
#ifdef DEBUG_LEGION
          assert(backward_map.find(dst_indexes[idx]) == backward_map.end());
#endif
          backward_map[dst_indexes[idx]] = src_indexes[idx];
        }
      }
      int index = dst_mask.find_first_set();
      while (index >= 0)
      {
#ifdef DEBUG_LEGION
        assert(backward_map.find(index) != backward_map.end());
#endif
        src_mask.set_bit(backward_map[index]);
        index = dst_mask.find_next_set(index+1);
      }
      return src_mask;
    }

    /////////////////////////////////////////////////////////////
    // Layout Description 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LayoutDescription::LayoutDescription(FieldSpaceNode *own,
                                         const FieldMask &mask,
                                         const unsigned dims,
                                         LayoutConstraints *con,
                                   const std::vector<unsigned> &mask_index_map,
                                   const std::vector<FieldID> &field_ids,
                                   const std::vector<size_t> &field_sizes,
                                   const std::vector<CustomSerdezID> &serdez)
      : allocated_fields(mask), constraints(con), owner(own), total_dims(dims)
    //--------------------------------------------------------------------------
    {
      constraints->add_base_gc_ref(LAYOUT_DESC_REF);
      field_infos.resize(field_sizes.size());
      // Switch data structures from layout by field order to order
      // of field locations in the bit mask
#ifdef DEBUG_LEGION
      // Greater than or equal because local fields can alias onto the
      // same index for the allocated instances, note that the fields
      // themselves still get allocated their own space in the instance
      assert(mask_index_map.size() >= 
                size_t(FieldMask::pop_count(allocated_fields)));
#endif
      for (unsigned idx = 0; idx < mask_index_map.size(); idx++)
      {
        // This gives us the index in the field ordered data structures
        unsigned index = mask_index_map[idx];
        FieldID fid = field_ids[index];
        field_indexes[fid] = idx;
        CopySrcDstField &info = field_infos[idx];
        info.size = field_sizes[index];
        info.field_id = fid;
        info.serdez_id = serdez[index];
      }
    }

    //--------------------------------------------------------------------------
    LayoutDescription::LayoutDescription(const FieldMask &mask,
                                         LayoutConstraints *con)
      : allocated_fields(mask), constraints(con), owner(NULL), total_dims(0)
    //--------------------------------------------------------------------------
    {
      constraints->add_base_gc_ref(LAYOUT_DESC_REF);
    }

    //--------------------------------------------------------------------------
    LayoutDescription::LayoutDescription(const LayoutDescription &rhs)
      : allocated_fields(rhs.allocated_fields), constraints(rhs.constraints), 
        owner(rhs.owner), total_dims(rhs.total_dims)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    LayoutDescription::~LayoutDescription(void)
    //--------------------------------------------------------------------------
    {
      comp_cache.clear();
      if (constraints->remove_base_gc_ref(LAYOUT_DESC_REF))
        delete (constraints);
    }

    //--------------------------------------------------------------------------
    LayoutDescription& LayoutDescription::operator=(
                                                   const LayoutDescription &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::log_instance_layout(ApEvent inst_event) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(implicit_runtime->legion_spy_enabled);
#endif
      for (std::map<FieldID,unsigned>::const_iterator it = 
            field_indexes.begin(); it != field_indexes.end(); it++)
        LegionSpy::log_physical_instance_field(inst_event, it->first);
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::compute_copy_offsets(const FieldMask &copy_mask,
                                                 PhysicalManager *manager,
                                           std::vector<CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
      uint64_t hash_key = copy_mask.get_hash_key();
      bool found_in_cache = false;
      FieldMask compressed;
      // First check to see if we've memoized this result 
      {
        AutoLock o_lock(layout_lock,1,false/*exclusive*/);
        std::map<LEGION_FIELD_MASK_FIELD_TYPE,
                 LegionList<std::pair<FieldMask,FieldMask> >::aligned>::
                   const_iterator finder = comp_cache.find(hash_key);
        if (finder != comp_cache.end())
        {
          for (LegionList<std::pair<FieldMask,FieldMask> >::aligned::
                const_iterator it = finder->second.begin(); 
                it != finder->second.end(); it++)
          {
            if (it->first == copy_mask)
            {
              found_in_cache = true;
              compressed = it->second;
              break;
            }
          }
        }
      }
      if (!found_in_cache)
      {
        compressed = copy_mask;
        compress_mask<STATIC_LOG2(LEGION_MAX_FIELDS)>(compressed, 
                                                      allocated_fields);
        // Save the result in the cache, duplicates from races here are benign
        AutoLock o_lock(layout_lock);
        comp_cache[hash_key].push_back(
            std::pair<FieldMask,FieldMask>(copy_mask,compressed));
      }
      // It is absolutely imperative that these infos be added in
      // the order in which they appear in the field mask so that 
      // they line up in the same order with the source/destination infos
      // (depending on the calling context of this function
      int pop_count = FieldMask::pop_count(compressed);
#ifdef DEBUG_LEGION
      assert(pop_count == FieldMask::pop_count(copy_mask));
#endif
      unsigned offset = fields.size();
      fields.resize(offset + pop_count);
      int next_start = 0;
      const PhysicalInstance instance = manager->instance;
#ifdef LEGION_SPY
      const ApEvent inst_event = manager->get_unique_event();
#endif
      for (int idx = 0; idx < pop_count; idx++)
      {
        int index = compressed.find_next_set(next_start);
        CopySrcDstField &field = fields[offset+idx];
        field = field_infos[index];
        // Our field infos are annonymous so specify the instance now
        field.inst = instance;
        // We'll start looking again at the next index after this one
        next_start = index + 1;
#ifdef LEGION_SPY
        field.inst_event = inst_event;
#endif
      }
    } 

    //--------------------------------------------------------------------------
    void LayoutDescription::compute_copy_offsets(
                                   const std::vector<FieldID> &copy_fields, 
                                   PhysicalManager *manager,
                                   std::vector<CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
      unsigned offset = fields.size();
      fields.resize(offset + copy_fields.size());
      const PhysicalInstance instance = manager->instance;
#ifdef LEGION_SPY
      const ApEvent inst_event = manager->get_unique_event();
#endif
      for (unsigned idx = 0; idx < copy_fields.size(); idx++)
      {
        std::map<FieldID,unsigned>::const_iterator
          finder = field_indexes.find(copy_fields[idx]);
#ifdef DEBUG_LEGION
        assert(finder != field_indexes.end());
#endif
        CopySrcDstField &info = fields[offset+idx];
        info = field_infos[finder->second];
        // Since instances are annonymous in layout descriptions we
        // have to fill them in when we add the field info
        info.inst = instance;
#ifdef LEGION_SPY
        info.inst_event = inst_event;
#endif
      }
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::get_fields(std::set<FieldID> &fields) const
    //--------------------------------------------------------------------------
    {
      for (std::map<FieldID,unsigned>::const_iterator 
	     it = field_indexes.begin(); it != field_indexes.end(); ++it)
	fields.insert(it->first);
    }

    //--------------------------------------------------------------------------
    bool LayoutDescription::has_field(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      return (field_indexes.find(fid) != field_indexes.end());
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::has_fields(std::map<FieldID,bool> &to_test) const
    //--------------------------------------------------------------------------
    {
      for (std::map<FieldID,bool>::iterator it = to_test.begin();
            it != to_test.end(); it++)
      {
        if (field_indexes.find(it->first) != field_indexes.end())
          it->second = true;
        else
          it->second = false;
      }
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::remove_space_fields(std::set<FieldID> &filter) const
    //--------------------------------------------------------------------------
    {
      std::vector<FieldID> to_remove;
      for (std::set<FieldID>::const_iterator it = filter.begin();
            it != filter.end(); it++)
      {
        if (field_indexes.find(*it) != field_indexes.end())
          to_remove.push_back(*it);
      }
      if (!to_remove.empty())
      {
        for (std::vector<FieldID>::const_iterator it = to_remove.begin();
              it != to_remove.end(); it++)
          filter.erase(*it);
      }
    }

    //--------------------------------------------------------------------------
    const CopySrcDstField& LayoutDescription::find_field_info(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      std::map<FieldID,unsigned>::const_iterator finder = 
        field_indexes.find(fid);
#ifdef DEBUG_LEGION
      assert(finder != field_indexes.end());
#endif
      return field_infos[finder->second];
    }

    //--------------------------------------------------------------------------
    size_t LayoutDescription::get_total_field_size(void) const
    //--------------------------------------------------------------------------
    {
      size_t result = 0;
      // Add up all the field sizes
      for (std::vector<CopySrcDstField>::const_iterator it = 
            field_infos.begin(); it != field_infos.end(); it++)
      {
        result += it->size;
      }
      return result;
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::get_fields(std::vector<FieldID>& fields) const
    //--------------------------------------------------------------------------
    {
      fields = constraints->field_constraint.get_field_set();
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::compute_destroyed_fields(
             std::vector<PhysicalInstance::DestroyedField> &serdez_fields) const
    //--------------------------------------------------------------------------
    {
      // See if we have any special fields which need serdez deletion
      for (std::vector<CopySrcDstField>::const_iterator it = 
            field_infos.begin(); it != field_infos.end(); it++)
      {
        if (it->serdez_id > 0)
          serdez_fields.push_back(PhysicalInstance::DestroyedField(it->field_id, 
                                                    it->size, it->serdez_id));
      }
    }

    //--------------------------------------------------------------------------
    bool LayoutDescription::match_layout(
      const LayoutConstraintSet &candidate_constraints, unsigned num_dims) const
    //--------------------------------------------------------------------------
    {
      if (num_dims != total_dims)
        return false;
      // We need to check equality on the entire constraint sets
      return *constraints == candidate_constraints;
    }

    //--------------------------------------------------------------------------
    bool LayoutDescription::match_layout(const LayoutDescription *layout,
                                         unsigned num_dims) const
    //--------------------------------------------------------------------------
    {
      if (num_dims != total_dims)
        return false;
      // This is a sound test, but it doesn't guarantee that the field sets
      // match since fields can be allocated and freed between instance
      // creations, so while this is a necessary precondition, it is not
      // sufficient that the two sets of fields are the same, to guarantee
      // that we actually need to check the FieldIDs which happens next
      if (layout->allocated_fields != allocated_fields)
        return false;

      // Check equality on the entire constraint sets
      return *layout->constraints == *constraints;
    }

    //--------------------------------------------------------------------------
    void LayoutDescription::pack_layout_description(Serializer &rez,
                                                    AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      rez.serialize(constraints->layout_id);
    }

    //--------------------------------------------------------------------------
    /*static*/ LayoutDescription* LayoutDescription::
      handle_unpack_layout_description(LayoutConstraints *constraints,
                            FieldSpaceNode *field_space_node, size_t total_dims)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(constraints != NULL);
#endif
      FieldMask instance_mask;
      const std::vector<FieldID> &field_set = 
        constraints->field_constraint.get_field_set(); 
      std::vector<size_t> field_sizes(field_set.size());
      std::vector<unsigned> mask_index_map(field_set.size());
      std::vector<CustomSerdezID> serdez(field_set.size());
      field_space_node->compute_field_layout(field_set, field_sizes,
                               mask_index_map, serdez, instance_mask);
      LayoutDescription *result = 
        field_space_node->create_layout_description(instance_mask, total_dims,
                  constraints, mask_index_map, field_set, field_sizes, serdez);
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      return result;
    }

    /////////////////////////////////////////////////////////////
    // PhysicalManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    PhysicalManager::PhysicalManager(RegionTreeForest *ctx,
                                     MemoryManager *memory, 
                                     LayoutDescription *desc,
                                     const PointerConstraint &constraint,
                                     DistributedID did,
                                     AddressSpaceID owner_space,
                                     FieldSpaceNode *node,
                                     PhysicalInstance inst, 
                                     size_t footprint, IndexSpaceExpression *d,
                                     RegionTreeID tid, bool register_now)
      : DistributedCollectable(ctx->runtime, did, owner_space, register_now), 
        context(ctx), memory_manager(memory), field_space_node(node), 
        layout(desc), instance(inst), instance_footprint(footprint),
        instance_domain(d), tree_id(tid), pointer_constraint(constraint)
    //--------------------------------------------------------------------------
    {
      if (field_space_node != NULL)
      {
        field_space_node->add_base_gc_ref(PHYSICAL_MANAGER_REF);
      }
      if (instance_domain != NULL)
        instance_domain->add_expression_reference();
      // Add a reference to the layout
      if (layout != NULL)
        layout->add_reference(); 
    }

    //--------------------------------------------------------------------------
    PhysicalManager::~PhysicalManager(void)
    //--------------------------------------------------------------------------
    {
      if (field_space_node != NULL)
      {
        if (field_space_node->remove_base_gc_ref(PHYSICAL_MANAGER_REF))
          delete field_space_node;
      }
      if ((instance_domain != NULL) && 
          instance_domain->remove_expression_reference())
        delete instance_domain;
      // Remote references removed by DistributedCollectable destructor
      if (!is_owner())
        memory_manager->unregister_remote_instance(this);
      if ((layout != NULL) && layout->remove_reference())
        delete layout;
      if (!gc_events.empty())
      {
        // There's no need to launch a task to do this, if we're being
        // deleted it's because the instance was deleted and therefore
        // all the users are done using it
        for (std::map<CollectableView*,CollectableInfo>::iterator it = 
              gc_events.begin(); it != gc_events.end(); it++)
        {
          if (it->second.collect_event.exists() &&
              !it->second.collect_event.has_triggered())
            it->second.collect_event.wait();
          CollectableView::handle_deferred_collect(it->first,
                                                   it->second.view_events);
        }
        gc_events.clear();
      }
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::log_instance_creation(UniqueID creator_id,
                Processor proc, const std::vector<LogicalRegion> &regions) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(runtime->legion_spy_enabled);
#endif
      const ApEvent inst_event = get_unique_event();
      LegionSpy::log_physical_instance_creator(inst_event, creator_id, proc.id);
      for (unsigned idx = 0; idx < regions.size(); idx++)
        LegionSpy::log_physical_instance_creation_region(inst_event, 
                                                         regions[idx]);
      const LayoutConstraints *constraints = layout->constraints;
      LegionSpy::log_instance_specialized_constraint(inst_event,
          constraints->specialized_constraint.kind, 
          constraints->specialized_constraint.redop);
#ifdef DEBUG_HIGH_LEVEL
      assert(constraints->memory_constraint.has_kind);
#endif
      if (constraints->memory_constraint.is_valid())
        LegionSpy::log_instance_memory_constraint(inst_event,
            constraints->memory_constraint.kind);
      LegionSpy::log_instance_field_constraint(inst_event,
          constraints->field_constraint.contiguous, 
          constraints->field_constraint.inorder,
          constraints->field_constraint.field_set.size());
      for (std::vector<FieldID>::const_iterator it = 
            constraints->field_constraint.field_set.begin(); it !=
            constraints->field_constraint.field_set.end(); it++)
        LegionSpy::log_instance_field_constraint_field(inst_event, *it);
      LegionSpy::log_instance_ordering_constraint(inst_event,
          constraints->ordering_constraint.contiguous,
          constraints->ordering_constraint.ordering.size());
      for (std::vector<DimensionKind>::const_iterator it = 
            constraints->ordering_constraint.ordering.begin(); it !=
            constraints->ordering_constraint.ordering.end(); it++)
        LegionSpy::log_instance_ordering_constraint_dimension(inst_event, *it);
      for (std::vector<SplittingConstraint>::const_iterator it = 
            constraints->splitting_constraints.begin(); it !=
            constraints->splitting_constraints.end(); it++)
        LegionSpy::log_instance_splitting_constraint(inst_event,
                                it->kind, it->value, it->chunks);
      for (std::vector<DimensionConstraint>::const_iterator it = 
            constraints->dimension_constraints.begin(); it !=
            constraints->dimension_constraints.end(); it++)
        LegionSpy::log_instance_dimension_constraint(inst_event,
                                    it->kind, it->eqk, it->value);
      for (std::vector<AlignmentConstraint>::const_iterator it = 
            constraints->alignment_constraints.begin(); it !=
            constraints->alignment_constraints.end(); it++)
        LegionSpy::log_instance_alignment_constraint(inst_event,
                                it->fid, it->eqk, it->alignment);
      for (std::vector<OffsetConstraint>::const_iterator it = 
            constraints->offset_constraints.begin(); it != 
            constraints->offset_constraints.end(); it++)
        LegionSpy::log_instance_offset_constraint(inst_event,
                                          it->fid, it->offset);
    } 

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_active(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      if (is_owner())
        assert(instance.exists());
#endif
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->activate_instance(this);
      // If we are not the owner, send a reference
      if (!is_owner())
        send_remote_gc_increment(owner_space, mutator);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_inactive(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      if (is_owner())
        assert(instance.exists());
#endif
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->deactivate_instance(this);
      if (!is_owner())
        send_remote_gc_decrement(owner_space, mutator);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_valid(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      // No need to do anything
#ifdef DEBUG_LEGION
      if (is_owner())
        assert(instance.exists());
#endif
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->validate_instance(this);
      // If we are not the owner, send a reference
      if (!is_owner())
        send_remote_valid_increment(owner_space, mutator);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::notify_invalid(ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      if (is_owner())
        assert(instance.exists());
#endif
      // If we have any gc events then launch tasks to actually prune
      // off their references when they are done since we are now eligible
      // for collection by the garbage collector
      // We can test this without the lock because the race here is with
      // the shutdown detection code (see find_shutdown_preconditions)
      // which is also only reading the data structure
      if (!gc_events.empty())
      {
        // We do need the lock if we're going to be modifying this
        AutoLock inst(inst_lock);
        for (std::map<CollectableView*,CollectableInfo>::iterator it =
              gc_events.begin(); it != gc_events.end(); it++)
        {
          GarbageCollectionArgs args(it->first, new std::set<ApEvent>());
          RtEvent precondition =
            Runtime::protect_merge_events(it->second.view_events);
          args.to_collect->swap(it->second.view_events);
          if (it->second.collect_event.exists() &&
              !it->second.collect_event.has_triggered())
            precondition = Runtime::merge_events(precondition, 
                                    it->second.collect_event);
          runtime->issue_runtime_meta_task(args, 
              LG_THROUGHPUT_WORK_PRIORITY, precondition);
        }
        gc_events.clear();
      }
      // Will be null for virtual managers
      if (memory_manager != NULL)
        memory_manager->invalidate_instance(this);
      if (!is_owner())
        send_remote_valid_decrement(owner_space, mutator);
    }

    //--------------------------------------------------------------------------
    ApEvent PhysicalManager::fill_from(FillView *fill_view,ApEvent precondition,
                                       PredEvent predicate_guard,
                                       IndexSpaceExpression *expression,
                                       const FieldMask &fill_mask,
                                       const PhysicalTraceInfo &trace_info,
                                       CopyAcrossHelper *across_helper,
                                       FieldMaskSet<FillView> *tracing_srcs,
                                       FieldMaskSet<InstanceView> *tracing_dsts)
    //--------------------------------------------------------------------------
    {
      // Implement in derived classes
      assert(false);
      return ApEvent::NO_AP_EVENT;
    }

    //--------------------------------------------------------------------------
    ApEvent PhysicalManager::copy_from(PhysicalManager *manager,
                                       ApEvent precondition,
                                       PredEvent predicate_guard, 
                                       ReductionOpID reduction_op,
                                       IndexSpaceExpression *expression,
                                       const FieldMask &copy_mask,
                                       const PhysicalTraceInfo &trace_info,
                                       CopyAcrossHelper *across_helper,
                                       FieldMaskSet<InstanceView> *tracing_srcs,
                                       FieldMaskSet<InstanceView> *tracing_dsts)
    //--------------------------------------------------------------------------
    {
      // Implement in derived classes
      assert(false);
      return ApEvent::NO_AP_EVENT;
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::compute_copy_offsets(const FieldMask &copy_mask,
                                           std::vector<CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(layout != NULL);
      assert(instance.exists());
#endif
      // Pass in our physical instance so the layout knows how to specialize
      layout->compute_copy_offsets(copy_mask, this, fields);
    }

    //--------------------------------------------------------------------------
    /*static*/void PhysicalManager::handle_manager_request(Deserializer &derez,
                                        Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      DistributedCollectable *dc = runtime->find_distributed_collectable(did);
#ifdef DEBUG_LEGION
      PhysicalManager *manager = dynamic_cast<PhysicalManager*>(dc);
      assert(manager != NULL);
#else
      PhysicalManager *manager = dynamic_cast<PhysicalManager*>(dc);
#endif
      manager->send_manager(source);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::register_active_context(InnerContext *context)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner()); // should always be on the owner node
#endif
      context->add_reference();
      AutoLock inst(inst_lock);
#ifdef DEBUG_LEGION
      assert(active_contexts.find(context) == active_contexts.end());
#endif
      active_contexts.insert(context);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::unregister_active_context(InnerContext *context)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner()); // should always be on the owner node
#endif
      {
        AutoLock inst(inst_lock);
        std::set<InnerContext*>::iterator finder = 
          active_contexts.find(context);
        // We could already have removed this context if this
        // physical instance was deleted
        if (finder == active_contexts.end())
          return;
        active_contexts.erase(finder);
      }
      if (context->remove_reference())
        delete context;
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::meets_region_tree(
                                const std::vector<LogicalRegion> &regions) const
    //--------------------------------------------------------------------------
    {
      for (std::vector<LogicalRegion>::const_iterator it = 
            regions.begin(); it != regions.end(); it++)
      {
        // Check to see if the region tree IDs are the same
        if (it->get_field_space() != tree_id)
          return false;
      }
      return true;
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::meets_regions(
      const std::vector<LogicalRegion> &regions, bool tight_region_bounds) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(tree_id > 0); // only happens with VirtualManager
      assert(!regions.empty());
#endif
      std::set<IndexSpaceExpression*> region_exprs;
      for (std::vector<LogicalRegion>::const_iterator it = 
            regions.begin(); it != regions.end(); it++)
      {
        // If the region tree IDs don't match that is bad
        if (it->get_tree_id() != tree_id)
          return false;
        RegionNode *node = context->get_node(*it);
        region_exprs.insert(node->row_source);
      }
      IndexSpaceExpression *space_expr = (region_exprs.size() == 1) ?
        *(region_exprs.begin()) : context->union_index_spaces(region_exprs);
      return meets_expression(space_expr, tight_region_bounds);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::meets_expression(IndexSpaceExpression *space_expr,
                                           bool tight_bounds) const
    //--------------------------------------------------------------------------
    {
      const size_t expr_volume = space_expr->get_volume();
      // If the space we need is empty then we're done for any instance
      if (expr_volume == 0)
        return true;
      const size_t inst_volume = instance_domain->get_volume();
      // If we don't even have enough volume there is now way to satisfy it
      if (inst_volume < expr_volume)
        return false;
      // Check to see if we have enough space in this instance
      IndexSpaceExpression *cover_expr = 
        context->subtract_index_spaces(space_expr, instance_domain);
      // If it's not empty then we don't have enough space
      if (!cover_expr->is_empty())
        return false;
      // We have enough space, if it's tight, then see if it is identical
      if (tight_bounds)
      {
        // We know we cover, so the only way we're tight are is if
        // we have exactly the same set of points which requires 
        // that the number of points be the same
        if (expr_volume == inst_volume)
          return true;
        else
          return false;
      }
      else
        // If we make it here then we have satisfied the expression 
        return true;
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::entails(LayoutConstraints *constraints,
                               const LayoutConstraint **failed_constraint) const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (!pointer_constraint.entails(constraints->pointer_constraint))
        return false;
      return layout->constraints->entails_without_pointer(constraints,
              (instance_domain != NULL) ? instance_domain->get_num_dims() : 0,
              failed_constraint);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::entails(const LayoutConstraintSet &constraints,
                               const LayoutConstraint **failed_constraint) const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (!pointer_constraint.entails(constraints.pointer_constraint))
        return false;
      return layout->constraints->entails_without_pointer(constraints,
              (instance_domain != NULL) ? instance_domain->get_num_dims() : 0,
              failed_constraint);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::conflicts(LayoutConstraints *constraints,
                             const LayoutConstraint **conflict_constraint) const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (pointer_constraint.conflicts(constraints->pointer_constraint))
        return true;
      // We know our layouts don't have a pointer constraint so nothing special
      return layout->constraints->conflicts(constraints,
              (instance_domain != NULL) ? instance_domain->get_num_dims() : 0,
              conflict_constraint);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::conflicts(const LayoutConstraintSet &constraints,
                             const LayoutConstraint **conflict_constraint) const
    //--------------------------------------------------------------------------
    {
      // Always test the pointer constraint locally
      if (pointer_constraint.conflicts(constraints.pointer_constraint))
        return true;
      // We know our layouts don't have a pointer constraint so nothing special
      return layout->constraints->conflicts(constraints,
              (instance_domain != NULL) ? instance_domain->get_num_dims() : 0,
              conflict_constraint);
    }

    //--------------------------------------------------------------------------
    bool PhysicalManager::acquire_instance(ReferenceSource source,
                                           ReferenceMutator *mutator)
    //--------------------------------------------------------------------------
    {
      // Do an atomic operation to check to see if we are already valid
      // and increment our count if we are, in this case the acquire 
      // has succeeded and we are done, this should be the common case
      // since we are likely already holding valid references elsewhere
      // Note that we cannot do this for external instances as they might
      // have been detached while still holding valid references so they
      // have to go through the full path every time
      if (!is_external_instance() && check_valid_and_increment(source))
        return true;
      // If we're not the owner, we're not going to succeed past this
      // since we aren't on the same node as where the instance lives
      // which is where the point of serialization is for garbage collection
      if (!is_owner())
        return false;
      // Tell our manager, we're attempting an acquire, if it tells
      // us false then we are not allowed to proceed
      if (!memory_manager->attempt_acquire(this))
        return false;
      // At this point we're in the clear to add our valid reference
      add_base_valid_ref(source, mutator);
      // Complete the handshake with the memory manager
      memory_manager->complete_acquire(this);
      return true;
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::perform_deletion(RtEvent deferred_event)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
#endif
      log_garbage.spew("Deleting physical instance " IDFMT " in memory " 
                       IDFMT "", instance.id, memory_manager->memory.id);
#ifndef DISABLE_GC
      std::vector<PhysicalInstance::DestroyedField> serdez_fields;
      layout->compute_destroyed_fields(serdez_fields); 
      if (!serdez_fields.empty())
        instance.destroy(serdez_fields, deferred_event);
      else
        instance.destroy(deferred_event);
#ifdef LEGION_MALLOC_INSTANCES
      if (!is_external_instance())
        memory_manager->free_legion_instance(this, deferred_event);
#endif
#endif
      // Notify any contexts of our deletion
      // Grab a copy of this in case we get any removal calls
      // while we are doing the deletion. We know that there
      // will be no more additions because we are being deleted
      std::set<InnerContext*> copy_active_contexts;
      {
        AutoLock inst(inst_lock);
        if (active_contexts.empty())
          return;
        copy_active_contexts = active_contexts;
        active_contexts.clear();
      }
      for (std::set<InnerContext*>::const_iterator it = 
           copy_active_contexts.begin(); it != copy_active_contexts.end(); it++)
      {
        (*it)->notify_instance_deletion(const_cast<PhysicalManager*>(this));
        if ((*it)->remove_reference())
          delete (*it);
      }
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::force_deletion(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
#endif
      log_garbage.spew("Force deleting physical instance " IDFMT " in memory "
                       IDFMT "", instance.id, memory_manager->memory.id);
#ifndef DISABLE_GC
      std::vector<PhysicalInstance::DestroyedField> serdez_fields;
      layout->compute_destroyed_fields(serdez_fields); 
      if (!serdez_fields.empty())
        instance.destroy(serdez_fields);
      else
        instance.destroy();
#ifdef LEGION_MALLOC_INSTANCES
      if (!is_external_instance())
        memory_manager->free_legion_instance(this, RtEvent::NO_RT_EVENT);
#endif
#endif
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::set_garbage_collection_priority(MapperID mapper_id,
                                            Processor proc, GCPriority priority)
    //--------------------------------------------------------------------------
    {
      memory_manager->set_garbage_collection_priority(this, mapper_id,
                                                      proc, priority);
    }

    //--------------------------------------------------------------------------
    RtEvent PhysicalManager::detach_external_instance(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_external_instance());
#endif
      return memory_manager->detach_external_instance(this);
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::defer_collect_user(CollectableView *view,
                                             ApEvent term_event,RtEvent collect,
                                             std::set<ApEvent> &to_collect,
                                             bool &add_ref, bool &remove_ref) 
    //--------------------------------------------------------------------------
    {
      AutoLock inst(inst_lock);
      CollectableInfo &info = gc_events[view]; 
      if (info.view_events.empty())
        add_ref = true;
      info.view_events.insert(term_event);
      info.events_added++;
      if (collect.exists())
        info.collect_event = collect;
      // Skip collections if there is a collection event guarding 
      // collection in the case of tracing
      if (info.collect_event.exists())
      {
        if (!info.collect_event.has_triggered())
          return;
        else
          info.collect_event = RtEvent::NO_RT_EVENT;
      }
      // Only do the pruning for every so many adds
      if (info.events_added >= runtime->gc_epoch_size)
      {
        for (std::set<ApEvent>::iterator it = info.view_events.begin();
              it != info.view_events.end(); /*nothing*/)
        {
          if (it->has_triggered())
          {
            to_collect.insert(*it);
            std::set<ApEvent>::iterator to_delete = it++;
            info.view_events.erase(to_delete);
          }
          else
            it++;
        }
        if (info.view_events.empty())
        {
          gc_events.erase(view);
          if (add_ref)
            add_ref = false;
          else
            remove_ref = true;
        }
        else // Reset the counter for the next time
          info.events_added = 0;
      }
    }

    //--------------------------------------------------------------------------
    void PhysicalManager::find_shutdown_preconditions(
                                               std::set<ApEvent> &preconditions)
    //--------------------------------------------------------------------------
    {
      AutoLock inst(inst_lock,1,false/*exclusive*/);
      for (std::map<CollectableView*,CollectableInfo>::const_iterator git =
            gc_events.begin(); git != gc_events.end(); git++)
      {
        // Make sure to test these for having triggered or risk a shutdown hang
        for (std::set<ApEvent>::const_iterator it = 
              git->second.view_events.begin(); it != 
              git->second.view_events.end(); it++)
          if (!it->has_triggered())
            preconditions.insert(*it);
      }
    }

    //--------------------------------------------------------------------------
    /*static*/ ApEvent PhysicalManager::fetch_metadata(PhysicalInstance inst, 
                                                       ApEvent use_event)
    //--------------------------------------------------------------------------
    {
      ApEvent ready(inst.fetch_metadata(Processor::get_executing_processor()));
      if (!use_event.exists())
        return ready;
      if (!ready.exists())
        return use_event;
      return Runtime::merge_events(NULL, ready, use_event);
    }

    /////////////////////////////////////////////////////////////
    // InstanceManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    InstanceManager::InstanceManager(RegionTreeForest *ctx, DistributedID did,
                                     AddressSpaceID owner_space, 
                                     MemoryManager *mem, PhysicalInstance inst,
                                     IndexSpaceExpression *instance_domain,
                                     FieldSpaceNode *node, RegionTreeID tid,
                                     LayoutDescription *desc,
                                     const PointerConstraint &constraint,
                                     bool register_now, size_t footprint,
                                     ApEvent u_event, bool external_instance)
      : PhysicalManager(ctx, mem, desc, constraint, 
                        encode_instance_did(did, external_instance),owner_space,
                        node, inst, footprint,instance_domain,tid,register_now),
        use_event(fetch_metadata(inst, u_event)), unique_event(u_event)
    //--------------------------------------------------------------------------
    {
      if (!is_owner())
      {
        // Register it with the memory manager, the memory manager
        // on the owner node will handle this
        memory_manager->register_remote_instance(this);
      } 
#ifdef LEGION_GC
      log_garbage.info("GC Instance Manager %lld %d " IDFMT " " IDFMT " ",
       LEGION_DISTRIBUTED_ID_FILTER(did), local_space, inst.id, mem->memory.id);
#endif
      if (runtime->legion_spy_enabled)
      {
#ifdef DEBUG_LEGION
        assert(unique_event.exists());
#endif
        LegionSpy::log_physical_instance(unique_event, inst.id, mem->memory.id, 
           instance_domain->expr_id, field_space_node->handle, tid, 0/*redop*/);
        layout->log_instance_layout(unique_event);
      }
    }

    //--------------------------------------------------------------------------
    InstanceManager::InstanceManager(const InstanceManager &rhs)
      : PhysicalManager(NULL, NULL, NULL, rhs.pointer_constraint, 0, 0, NULL,
                        PhysicalInstance::NO_INST, 0, NULL, 0, false),
        use_event(ApEvent::NO_AP_EVENT), unique_event(ApEvent::NO_AP_EVENT)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    InstanceManager::~InstanceManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    InstanceManager& InstanceManager::operator=(const InstanceManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        InstanceManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(instance.exists());
#endif
      return LegionRuntime::Accessor::RegionAccessor<
	LegionRuntime::Accessor::AccessorType::Generic>(instance);
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        InstanceManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(instance.exists());
      assert(layout != NULL);
#endif
      const CopySrcDstField &info = layout->find_field_info(fid);
      LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic> temp(instance);
      return temp.get_untyped_field_accessor(info.field_id, info.size);
    }

    //--------------------------------------------------------------------------
    ApEvent InstanceManager::fill_from(FillView *fill_view,ApEvent precondition,
                                       PredEvent predicate_guard,
                                       IndexSpaceExpression *fill_expression,
                                       const FieldMask &fill_mask,
                                       const PhysicalTraceInfo &trace_info,
                                       CopyAcrossHelper *across_helper,
                                       FieldMaskSet<FillView> *tracing_srcs,
                                       FieldMaskSet<InstanceView> *tracing_dsts)
    //--------------------------------------------------------------------------
    {
      std::vector<CopySrcDstField> dst_fields;
      if (across_helper == NULL)
        compute_copy_offsets(fill_mask, dst_fields); 
      else
        across_helper->compute_across_offsets(fill_mask, dst_fields);
      return fill_expression->issue_fill(trace_info, dst_fields, 
                                         fill_view->value->value,
                                         fill_view->value->value_size,
#ifdef LEGION_SPY
                                         fill_view->fill_op_uid,
                                         field_space_node->handle,
                                         tree_id,
#endif
                                         precondition, predicate_guard,
                                         tracing_srcs, tracing_dsts);
    }

    //--------------------------------------------------------------------------
    ApEvent InstanceManager::copy_from(PhysicalManager *source_manager,
                                       ApEvent precondition,
                                       PredEvent predicate_guard, 
                                       ReductionOpID reduction_op,
                                       IndexSpaceExpression *copy_expression,
                                       const FieldMask &copy_mask,
                                       const PhysicalTraceInfo &trace_info,
                                       CopyAcrossHelper *across_helper,
                                       FieldMaskSet<InstanceView> *tracing_srcs,
                                       FieldMaskSet<InstanceView> *tracing_dsts)
    //--------------------------------------------------------------------------
    {
      std::vector<CopySrcDstField> dst_fields, src_fields;
      if (across_helper == NULL)
        compute_copy_offsets(copy_mask, dst_fields); 
      else
        across_helper->compute_across_offsets(copy_mask, dst_fields);
      source_manager->compute_copy_offsets(copy_mask, src_fields);
      return copy_expression->issue_copy(trace_info, dst_fields, src_fields,
#ifdef LEGION_SPY
                                         source_manager->tree_id, tree_id,
#endif
                                         precondition, predicate_guard,
                                         reduction_op, false/*fold*/, 
                                         tracing_srcs, tracing_dsts);
    }

    //--------------------------------------------------------------------------
    InstanceView* InstanceManager::create_instance_top_view(
                            InnerContext *own_ctx, AddressSpaceID logical_owner)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
#endif
      DistributedID view_did = 
        context->runtime->get_available_distributed_id();
      UniqueID context_uid = own_ctx->get_context_uid();
      InstanceView* result = 
            new MaterializedView(context, view_did, owner_space, logical_owner,
                                 const_cast<InstanceManager*>(this),
                                 context_uid, true/*register now*/);
      register_active_context(own_ctx);
      return result;
    } 

    //--------------------------------------------------------------------------
    void InstanceManager::initialize_across_helper(CopyAcrossHelper *helper,
                                                   const FieldMask &dst_mask,
                                     const std::vector<unsigned> &src_indexes,
                                     const std::vector<unsigned> &dst_indexes)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(src_indexes.size() == dst_indexes.size());
#endif
      std::vector<CopySrcDstField> dst_fields;
      layout->compute_copy_offsets(dst_mask, this, dst_fields);
#ifdef DEBUG_LEGION
      assert(dst_fields.size() == dst_indexes.size());
#endif
      helper->offsets.resize(dst_fields.size());
      // We've got the offsets compressed based on their destination mask
      // order, now we need to translate them to their source mask order
      // Figure out the permutation from destination mask ordering to 
      // source mask ordering. 
      // First let's figure out the order of the source indexes
      std::vector<unsigned> src_order(src_indexes.size());
      std::map<unsigned,unsigned> translate_map;
      for (unsigned idx = 0; idx < src_indexes.size(); idx++)
        translate_map[src_indexes[idx]] = idx;
      unsigned index = 0;
      for (std::map<unsigned,unsigned>::const_iterator it = 
            translate_map.begin(); it != translate_map.end(); it++, index++)
        src_order[it->second] = index; 
      // Now we can translate the destination indexes
      translate_map.clear();
      for (unsigned idx = 0; idx < dst_indexes.size(); idx++)
        translate_map[dst_indexes[idx]] = idx;
      index = 0; 
      for (std::map<unsigned,unsigned>::const_iterator it = 
            translate_map.begin(); it != translate_map.end(); it++, index++)
      {
        unsigned src_index = src_order[it->second];
        helper->offsets[src_index] = dst_fields[index];
      }
    }

    //--------------------------------------------------------------------------
    void InstanceManager::send_manager(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
#endif
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(did);
        rez.serialize(owner_space);
        rez.serialize(memory_manager->memory);
        rez.serialize(instance);
        rez.serialize(instance_footprint);
        instance_domain->pack_expression(rez, target);
        rez.serialize(field_space_node->handle);
        rez.serialize(tree_id);
        rez.serialize(unique_event);
        layout->pack_layout_description(rez, target);
        pointer_constraint.serialize(rez);
      }
      context->runtime->send_instance_manager(target, rez);
      update_remote_instances(target);
    }

    //--------------------------------------------------------------------------
    /*static*/ void InstanceManager::handle_send_manager(Runtime *runtime, 
                                     AddressSpaceID source, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      AddressSpaceID owner_space;
      derez.deserialize(owner_space);
      Memory mem;
      derez.deserialize(mem);
      PhysicalInstance inst;
      derez.deserialize(inst);
      size_t inst_footprint;
      derez.deserialize(inst_footprint);
      bool local_is, domain_is;
      IndexSpace domain_handle;
      IndexSpaceExprID domain_expr_id;
      RtEvent domain_ready;
      IndexSpaceExpression *inst_domain = 
        IndexSpaceExpression::unpack_expression(derez, runtime->forest, source,
              local_is, domain_is, domain_handle, domain_expr_id, domain_ready);
      FieldSpace handle;
      derez.deserialize(handle);
      RtEvent fs_ready;
      FieldSpaceNode *space_node = runtime->forest->get_node(handle, &fs_ready);
      RegionTreeID tree_id;
      derez.deserialize(tree_id);
      ApEvent unique_event;
      derez.deserialize(unique_event);
      LayoutConstraintID layout_id;
      derez.deserialize(layout_id);
      RtEvent layout_ready;
      LayoutConstraints *constraints = 
        runtime->find_layout_constraints(layout_id, 
                    false/*can fail*/, &layout_ready);
      PointerConstraint pointer_constraint;
      pointer_constraint.deserialize(derez);
      if (domain_ready.exists() || fs_ready.exists() || layout_ready.exists())
      {
        const RtEvent precondition = 
          Runtime::merge_events(domain_ready, fs_ready, layout_ready);
        if (precondition.exists() && !precondition.has_triggered())
        {
          // We need to defer this instance creation
          DeferInstanceManagerArgs args(did, owner_space, mem, inst,
              inst_footprint, local_is, inst_domain, domain_is, domain_handle, 
              domain_expr_id, handle, tree_id, layout_id, pointer_constraint, 
              unique_event);
          runtime->issue_runtime_meta_task(args,
              LG_LATENCY_RESPONSE_PRIORITY, precondition);
          return;
        }
        // If we fall through we need to refetch things that we didn't get
        if (domain_ready.exists())
          inst_domain = domain_is ? 
            runtime->forest->get_node(domain_handle) :
            runtime->forest->find_remote_expression(domain_expr_id);
        if (fs_ready.exists())
          space_node = runtime->forest->get_node(handle);
        if (layout_ready.exists())
          constraints = 
            runtime->find_layout_constraints(layout_id, false/*can fail*/);
      }
      // If we fall through here we can create the manager now
      create_remote_manager(runtime, did, owner_space, mem, inst,inst_footprint,
                            inst_domain, space_node, tree_id, constraints, 
                            unique_event, pointer_constraint);
    }

    //--------------------------------------------------------------------------
    InstanceManager::DeferInstanceManagerArgs::DeferInstanceManagerArgs(
            DistributedID d, AddressSpaceID own, Memory m, PhysicalInstance i, 
            size_t f, bool local, IndexSpaceExpression *lx, bool is, 
            IndexSpace dh, IndexSpaceExprID dx, FieldSpace h, RegionTreeID tid,
            LayoutConstraintID l, PointerConstraint &p, ApEvent use)
      : LgTaskArgs<DeferInstanceManagerArgs>(implicit_provenance),
            did(d), owner(own), mem(m), inst(i), footprint(f), local_is(local),
            domain_is(is), local_expr(local ? lx : NULL), domain_handle(dh), 
            domain_expr(dx), handle(h), tree_id(tid), layout_id(l), 
            pointer(new PointerConstraint(p)), use_event(use)
    //--------------------------------------------------------------------------
    {
      if (local_is)
        local_expr->add_expression_reference();
    }

    //--------------------------------------------------------------------------
    /*static*/ void InstanceManager::handle_defer_manager(const void *args,
                                                          Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      const DeferInstanceManagerArgs *dargs = 
        (const DeferInstanceManagerArgs*)args; 
      IndexSpaceExpression *inst_domain = dargs->local_is ? dargs->local_expr :
        dargs->domain_is ? runtime->forest->get_node(dargs->domain_handle) :
        runtime->forest->find_remote_expression(dargs->domain_expr);
      FieldSpaceNode *space_node = runtime->forest->get_node(dargs->handle);
      LayoutConstraints *constraints = 
        runtime->find_layout_constraints(dargs->layout_id);
      create_remote_manager(runtime, dargs->did, dargs->owner, dargs->mem,
          dargs->inst, dargs->footprint, inst_domain, space_node, 
          dargs->tree_id, constraints, dargs->use_event, *(dargs->pointer));
      // Free up the pointer memory
      delete dargs->pointer;
      // Remove the local expression reference if necessary
      if (dargs->local_is && dargs->local_expr->remove_expression_reference())
        delete dargs->local_expr;
    }

    //--------------------------------------------------------------------------
    /*static*/ void InstanceManager::create_remote_manager(Runtime *runtime, 
          DistributedID did, AddressSpaceID owner_space, Memory mem, 
          PhysicalInstance inst, size_t inst_footprint, 
          IndexSpaceExpression *inst_domain, FieldSpaceNode *space_node, 
          RegionTreeID tree_id, LayoutConstraints *constraints, 
          ApEvent use_event, PointerConstraint &pointer_constraint)
    //--------------------------------------------------------------------------
    {
      LayoutDescription *layout = 
        LayoutDescription::handle_unpack_layout_description(constraints,
                                space_node, inst_domain->get_num_dims());
      MemoryManager *memory = runtime->find_memory_manager(mem);
      void *location;
      InstanceManager *man = NULL;
      const bool external_instance = PhysicalManager::is_external_did(did);
      if (runtime->find_pending_collectable_location(did, location))
        man = new(location) InstanceManager(runtime->forest,did,
                                            owner_space, memory, inst, 
                                            inst_domain, space_node, tree_id, 
                                            layout, pointer_constraint,
                                            false/*reg now*/, inst_footprint,
                                            use_event, external_instance); 
      else
        man = new InstanceManager(runtime->forest, did, owner_space, memory, 
                                  inst, inst_domain, space_node, tree_id, 
                                  layout, pointer_constraint, false/*reg now*/, 
                                  inst_footprint, use_event, external_instance);
      // Hold-off doing the registration until construction is complete
      man->register_with_runtime(NULL/*no remote registration needed*/);
    }

    /////////////////////////////////////////////////////////////
    // ReductionManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    ReductionManager::ReductionManager(RegionTreeForest *ctx, DistributedID did,
                                       AddressSpaceID owner_space, 
                                       MemoryManager *mem,PhysicalInstance inst,
                                       LayoutDescription *desc, 
                                       const PointerConstraint &constraint,
                                       IndexSpaceExpression *inst_domain,
                                       FieldSpaceNode *node, RegionTreeID tid,
                                       ReductionOpID red, const ReductionOp *o,
                                       ApEvent u_event, size_t footprint,
                                       bool register_now)
      : PhysicalManager(ctx, mem, desc, constraint, did, owner_space, 
                        node, inst, footprint, inst_domain, tid, register_now),
        op(o), redop(red), use_event(fetch_metadata(inst, u_event)), 
        unique_event(u_event)
    //--------------------------------------------------------------------------
    {  
      if (runtime->legion_spy_enabled)
      {
#ifdef DEBUG_LEGION
        assert(use_event.exists());
#endif
        LegionSpy::log_physical_instance(u_event, inst.id, mem->memory.id, 
                            inst_domain->expr_id, node->handle, tid, redop);
        layout->log_instance_layout(u_event);
      }
    }

    //--------------------------------------------------------------------------
    ReductionManager::~ReductionManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    void ReductionManager::send_manager(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
#endif
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(did);
        rez.serialize(owner_space);
        rez.serialize(memory_manager->memory);
        rez.serialize(instance);
        rez.serialize(instance_footprint);
        instance_domain->pack_expression(rez, target);
        rez.serialize(redop);
        rez.serialize(field_space_node->handle);
        rez.serialize(tree_id);
        rez.serialize<bool>(is_foldable());
        rez.serialize(get_pointer_space());
        rez.serialize(unique_event);
        layout->pack_layout_description(rez, target);
        pointer_constraint.serialize(rez);
      }
      // Now send the message
      context->runtime->send_reduction_manager(target, rez);
      update_remote_instances(target);
    }

    //--------------------------------------------------------------------------
    /*static*/ void ReductionManager::handle_send_manager(Runtime *runtime, 
                                     AddressSpaceID source, Deserializer &derez)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID did;
      derez.deserialize(did);
      AddressSpaceID owner_space;
      derez.deserialize(owner_space);
      Memory mem;
      derez.deserialize(mem);
      PhysicalInstance inst;
      derez.deserialize(inst);
      size_t inst_footprint;
      derez.deserialize(inst_footprint);
      bool local_is, domain_is;
      IndexSpace domain_handle;
      IndexSpaceExprID domain_expr_id;
      RtEvent domain_ready;
      IndexSpaceExpression *inst_domain = 
        IndexSpaceExpression::unpack_expression(derez, runtime->forest, source,
            local_is, domain_is, domain_handle, domain_expr_id, domain_ready);
      ReductionOpID redop;
      derez.deserialize(redop);
      FieldSpace handle;
      derez.deserialize(handle);
      RtEvent fs_ready;
      FieldSpaceNode *field_node = runtime->forest->get_node(handle, &fs_ready);
      RegionTreeID tree_id;
      derez.deserialize(tree_id);
      bool foldable;
      derez.deserialize(foldable);
      Domain ptr_space;
      derez.deserialize(ptr_space);
      ApEvent unique_event;
      derez.deserialize(unique_event);
      LayoutConstraintID layout_id;
      derez.deserialize(layout_id);
      RtEvent layout_ready;
      LayoutConstraints *constraints = 
        runtime->find_layout_constraints(layout_id, 
                    false/*can fail*/, &layout_ready);
      PointerConstraint pointer_constraint;
      pointer_constraint.deserialize(derez);
      if (domain_ready.exists() || fs_ready.exists() || layout_ready.exists())
      {
        const RtEvent precondition = 
          Runtime::merge_events(domain_ready, fs_ready, layout_ready);
        if (precondition.exists() && !precondition.has_triggered())
        {
          // We need to defer this instance creation
          DeferReductionManagerArgs args(did, owner_space, mem, inst,
              inst_footprint, local_is, inst_domain, domain_is, 
              domain_handle, domain_expr_id, handle, tree_id, layout_id, 
              pointer_constraint, unique_event, foldable, ptr_space, redop);
          runtime->issue_runtime_meta_task(args,
              LG_LATENCY_RESPONSE_PRIORITY, precondition);
          return;
        }
        // If we fall through we need to refetch things that we didn't get
        if (domain_ready.exists())
          inst_domain = domain_is ? 
            runtime->forest->get_node(domain_handle) :
            runtime->forest->find_remote_expression(domain_expr_id);
        if (fs_ready.exists())
          field_node = runtime->forest->get_node(handle);
        if (layout_ready.exists())
          constraints = 
            runtime->find_layout_constraints(layout_id, false/*can fail*/);
      }
      // If we fall through here we can create the manager now
      create_remote_manager(runtime, did, owner_space, mem, inst, 
          inst_footprint, inst_domain, field_node, tree_id, constraints,
          unique_event, pointer_constraint, foldable, ptr_space, redop);
    }

    //--------------------------------------------------------------------------
    ReductionManager::DeferReductionManagerArgs::DeferReductionManagerArgs(
            DistributedID d, AddressSpaceID own, Memory m,
            PhysicalInstance i, size_t f, bool local, IndexSpaceExpression *lx,
            bool is, IndexSpace dh, IndexSpaceExprID dx, FieldSpace h, 
            RegionTreeID tid, LayoutConstraintID l, PointerConstraint &p, 
            ApEvent use, bool fold, const Domain &ptr, ReductionOpID r)
      : LgTaskArgs<DeferReductionManagerArgs>(implicit_provenance),
            did(d), owner(own), mem(m), inst(i), footprint(f), local_is(local),
            domain_is(is), local_expr(local ? lx : NULL), domain_handle(dh), 
            domain_expr(dx), handle(h), tree_id(tid), layout_id(l), 
            pointer(new PointerConstraint(p)), use_event(use), foldable(fold), 
            ptr_space(ptr), redop(r)
    //--------------------------------------------------------------------------
    {
      if (local_is)
        local_expr->add_expression_reference();
    }

    //--------------------------------------------------------------------------
    /*static*/ void ReductionManager::handle_defer_manager(const void *args,
                                                           Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      const DeferReductionManagerArgs *dargs = 
        (const DeferReductionManagerArgs*)args; 
      IndexSpaceExpression *inst_domain = dargs->local_is ? dargs->local_expr :
        dargs->domain_is ? runtime->forest->get_node(dargs->domain_handle) :
        runtime->forest->find_remote_expression(dargs->domain_expr);
      FieldSpaceNode *space_node = runtime->forest->get_node(dargs->handle);
      LayoutConstraints *constraints = 
        runtime->find_layout_constraints(dargs->layout_id);
      create_remote_manager(runtime, dargs->did, dargs->owner, dargs->mem,
          dargs->inst, dargs->footprint, inst_domain, space_node, 
          dargs->tree_id, constraints, dargs->use_event, *(dargs->pointer),
          dargs->foldable, dargs->ptr_space, dargs->redop);
      // Free up the pointer memory
      delete dargs->pointer;
      // Remove the local expression reference if necessary
      if (dargs->local_is && dargs->local_expr->remove_expression_reference())
        delete dargs->local_expr;
    }

    //--------------------------------------------------------------------------
    /*static*/ void ReductionManager::create_remote_manager(Runtime *runtime, 
          DistributedID did, AddressSpaceID owner_space, Memory mem, 
          PhysicalInstance inst, size_t inst_footprint, 
          IndexSpaceExpression *inst_domain, FieldSpaceNode *space_node, 
          RegionTreeID tree_id, LayoutConstraints *constraints, 
          ApEvent use_event, PointerConstraint &pointer_constraint, 
          bool foldable, const Domain &ptr_space, ReductionOpID redop)
    //--------------------------------------------------------------------------
    {
      LayoutDescription *layout = 
        LayoutDescription::handle_unpack_layout_description(constraints,
                                space_node, inst_domain->get_num_dims());
      MemoryManager *memory = runtime->find_memory_manager(mem);
      const ReductionOp *op = Runtime::get_reduction_op(redop);
      ReductionManager *man = NULL;
      if (foldable)
      {
        void *location;
        if (runtime->find_pending_collectable_location(did, location))
          man = new(location) FoldReductionManager(runtime->forest,
                                                   did, owner_space,
                                                   memory, inst, layout,
                                                   pointer_constraint, 
                                                   inst_domain, space_node,
                                                   tree_id, redop, op,
                                                   use_event, inst_footprint,
                                                   false/*reg now*/);
        else
          man = new FoldReductionManager(runtime->forest, 
                                         did, owner_space, memory, inst,
                                         layout, pointer_constraint, 
                                         inst_domain, space_node, tree_id, 
                                         redop, op, use_event, inst_footprint,
                                         false/*reg now*/);
      }
      else
      {
        void *location;
        if (runtime->find_pending_collectable_location(did, location))
          man = new(location) ListReductionManager(runtime->forest,
                                                   did, owner_space, 
                                                   memory, inst, layout,
                                                   pointer_constraint, 
                                                   inst_domain, space_node,
                                                   tree_id, redop,
                                                   op, ptr_space, 
                                                   use_event, inst_footprint,
                                                   false/*reg now*/);
        else
          man = new ListReductionManager(runtime->forest, did, 
                                         owner_space, memory, inst,
                                         layout, pointer_constraint, 
                                         inst_domain, space_node, 
                                         tree_id, redop, op,
                                         ptr_space, use_event,
                                         inst_footprint, false/*reg now*/);
      }
      man->register_with_runtime(NULL/*no remote registration needed*/);
    }

    //--------------------------------------------------------------------------
    InstanceView* ReductionManager::create_instance_top_view(
                            InnerContext *own_ctx, AddressSpaceID logical_owner)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(is_owner());
#endif
      DistributedID view_did = 
        context->runtime->get_available_distributed_id();
      UniqueID context_uid = own_ctx->get_context_uid();
      InstanceView *result = 
             new ReductionView(context, view_did, owner_space, logical_owner,
                               const_cast<ReductionManager*>(this),
                               context_uid, true/*register now*/);
      register_active_context(own_ctx);
      return result;
    }

    /////////////////////////////////////////////////////////////
    // ListReductionManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    ListReductionManager::ListReductionManager(RegionTreeForest *ctx, 
                                               DistributedID did,
                                               AddressSpaceID owner_space, 
                                               MemoryManager *mem,
                                               PhysicalInstance inst, 
                                               LayoutDescription *desc,
                                               const PointerConstraint &cons,
                                               IndexSpaceExpression *d,
                                               FieldSpaceNode *node,
                                               RegionTreeID tid,
                                               ReductionOpID red,
                                               const ReductionOp *o, 
                                               Domain dom, ApEvent use_event, 
                                               size_t footprint,
                                               bool register_now)
      : ReductionManager(ctx, encode_reduction_list_did(did), owner_space, 
                         mem, inst, desc, cons, d, node, tid, red, o, 
                         use_event, footprint, register_now), ptr_space(dom)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(dom.is_id == 0); // shouldn't have a sparsity map for dom
#endif
      if (!is_owner())
      {
        // Register it with the memory manager, the memory manager
        // on the owner node will handle this
        memory_manager->register_remote_instance(this);
      }
#ifdef LEGION_GC
      log_garbage.info("GC List Reduction Manager %lld %d " IDFMT " " IDFMT " ",
       LEGION_DISTRIBUTED_ID_FILTER(did), local_space, inst.id, mem->memory.id);
#endif
    }

    //--------------------------------------------------------------------------
    ListReductionManager::ListReductionManager(const ListReductionManager &rhs)
      : ReductionManager(NULL, 0, 0, NULL,
                         PhysicalInstance::NO_INST, NULL,rhs.pointer_constraint,
                         NULL, NULL, 0, 0, NULL, ApEvent::NO_AP_EVENT, 0,false),
        ptr_space(Domain::NO_DOMAIN)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    ListReductionManager::~ListReductionManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    ListReductionManager& ListReductionManager::operator=(
                                                const ListReductionManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        ListReductionManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
      // TODO: Implement this 
      assert(false);
      return LegionRuntime::Accessor::RegionAccessor<
	LegionRuntime::Accessor::AccessorType::Generic>(instance);
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        ListReductionManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return LegionRuntime::Accessor::RegionAccessor<
	LegionRuntime::Accessor::AccessorType::Generic>(instance);
    }

    //--------------------------------------------------------------------------
    bool ListReductionManager::is_foldable(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    void ListReductionManager::compute_copy_offsets(const FieldMask &copy_mask,
                                           std::vector<CopySrcDstField> &fields)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(instance.exists());
#endif
      // TODO: implement this for list reduction instances
      assert(false);
    }

    //--------------------------------------------------------------------------
    Domain ListReductionManager::get_pointer_space(void) const
    //--------------------------------------------------------------------------
    {
      return ptr_space;
    }

    //--------------------------------------------------------------------------
    ApEvent ListReductionManager::copy_from(PhysicalManager *source_manager,
                                       ApEvent precondition,
                                       PredEvent predicate_guard, 
                                       ReductionOpID reduction_op,
                                       IndexSpaceExpression *copy_expression,
                                       const FieldMask &copy_mask,
                                       const PhysicalTraceInfo &trace_info,
                                       CopyAcrossHelper *across_helper,
                                       FieldMaskSet<InstanceView> *tracing_srcs,
                                       FieldMaskSet<InstanceView> *tracing_dsts)
    //--------------------------------------------------------------------------
    {
      // TODO: implement this for list reductions
      assert(false);
      return ApEvent::NO_AP_EVENT;
    }

    /////////////////////////////////////////////////////////////
    // FoldReductionManager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    FoldReductionManager::FoldReductionManager(RegionTreeForest *ctx, 
                                               DistributedID did,
                                               AddressSpaceID owner_space, 
                                               MemoryManager *mem,
                                               PhysicalInstance inst, 
                                               LayoutDescription *desc,
                                               const PointerConstraint &cons,
                                               IndexSpaceExpression *d,
                                               FieldSpaceNode *node,
                                               RegionTreeID tid,
                                               ReductionOpID red,
                                               const ReductionOp *o,
                                               ApEvent u_event,
                                               size_t footprint,
                                               bool register_now)
      : ReductionManager(ctx, encode_reduction_fold_did(did), owner_space, 
                         mem, inst, desc, cons, d, node, tid,
                         red, o, u_event, footprint, register_now)
    //--------------------------------------------------------------------------
    {
      if (!is_owner())
      {
        // Register it with the memory manager, the memory manager
        // on the owner node will handle this
        memory_manager->register_remote_instance(this);
      }
#ifdef LEGION_GC
      log_garbage.info("GC Fold Reduction Manager %lld %d " IDFMT " " IDFMT " ",
       LEGION_DISTRIBUTED_ID_FILTER(did), local_space, inst.id, mem->memory.id);
#endif
    }

    //--------------------------------------------------------------------------
    FoldReductionManager::FoldReductionManager(const FoldReductionManager &rhs)
      : ReductionManager(NULL, 0, 0, NULL,
                         PhysicalInstance::NO_INST, NULL,rhs.pointer_constraint,
                         NULL, NULL, 0, 0, NULL, ApEvent::NO_AP_EVENT, 0, false)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    FoldReductionManager::~FoldReductionManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    FoldReductionManager& FoldReductionManager::operator=(
                                                const FoldReductionManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        FoldReductionManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(instance.exists());
#endif
      return LegionRuntime::Accessor::RegionAccessor<
	LegionRuntime::Accessor::AccessorType::Generic>(instance);
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
      LegionRuntime::Accessor::AccessorType::Generic>
        FoldReductionManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(instance.exists());
      assert(layout != NULL);
#endif
      const CopySrcDstField &info = layout->find_field_info(fid);
      LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic> temp(instance);
      return temp.get_untyped_field_accessor(info.field_id, info.size);
    }

    //--------------------------------------------------------------------------
    bool FoldReductionManager::is_foldable(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

    //--------------------------------------------------------------------------
    Domain FoldReductionManager::get_pointer_space(void) const
    //--------------------------------------------------------------------------
    {
      return Domain::NO_DOMAIN;
    }

    //--------------------------------------------------------------------------
    ApEvent FoldReductionManager::copy_from(PhysicalManager *source_manager,
                                       ApEvent precondition,
                                       PredEvent predicate_guard, 
                                       ReductionOpID reduction_op,
                                       IndexSpaceExpression *copy_expression,
                                       const FieldMask &copy_mask,
                                       const PhysicalTraceInfo &trace_info,
                                       CopyAcrossHelper *across_helper,
                                       FieldMaskSet<InstanceView> *tracing_srcs,
                                       FieldMaskSet<InstanceView> *tracing_dsts)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(redop == reduction_op);
#endif
      std::vector<CopySrcDstField> dst_fields, src_fields;
      if (across_helper == NULL)
        compute_copy_offsets(copy_mask, dst_fields); 
      else
        across_helper->compute_across_offsets(copy_mask, dst_fields);
      source_manager->compute_copy_offsets(copy_mask, src_fields);
      return copy_expression->issue_copy(trace_info, dst_fields, src_fields,
#ifdef LEGION_SPY
                                         source_manager->tree_id, tree_id,
#endif
                                         precondition, predicate_guard,
                                         reduction_op, true/*fold*/, 
                                         tracing_srcs, tracing_dsts);
    }

    /////////////////////////////////////////////////////////////
    // Virtual Manager 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    VirtualManager::VirtualManager(RegionTreeForest *ctx, 
                                   LayoutDescription *desc,
                                   const PointerConstraint &constraint,
                                   DistributedID did)
      : PhysicalManager(ctx, NULL/*memory*/, desc, constraint, did, 
                        ctx->runtime->address_space,
                        NULL/*field space */, PhysicalInstance::NO_INST, 0,
                        NULL/*index space expression*/, 0, true/*reg now*/)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    VirtualManager::VirtualManager(const VirtualManager &rhs)
      : PhysicalManager(NULL, NULL, NULL, rhs.pointer_constraint, 0, 0,
                        NULL, PhysicalInstance::NO_INST, 0, NULL, 0, false)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    VirtualManager::~VirtualManager(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    VirtualManager& VirtualManager::operator=(const VirtualManager &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          VirtualManager::get_accessor(void) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
	(PhysicalInstance::NO_INST);
    }

    //--------------------------------------------------------------------------
    LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
          VirtualManager::get_field_accessor(FieldID fid) const
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return LegionRuntime::Accessor::RegionAccessor<
        LegionRuntime::Accessor::AccessorType::Generic>
	(PhysicalInstance::NO_INST);
    }

    //--------------------------------------------------------------------------
    ApEvent VirtualManager::get_use_event(void) const
    //--------------------------------------------------------------------------
    {
      return ApEvent::NO_AP_EVENT;
    }

    //--------------------------------------------------------------------------
    ApEvent VirtualManager::get_unique_event(void) const
    //--------------------------------------------------------------------------
    {
      return ApEvent::NO_AP_EVENT;
    }

    //--------------------------------------------------------------------------
    void VirtualManager::send_manager(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    InstanceView* VirtualManager::create_instance_top_view(
                            InnerContext *context, AddressSpaceID logical_owner)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return NULL;
    }

    /////////////////////////////////////////////////////////////
    // Instance Builder
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    InstanceBuilder::~InstanceBuilder(void)
    //--------------------------------------------------------------------------
    {
      if (realm_layout != NULL)
        delete realm_layout;
    }

    //--------------------------------------------------------------------------
    PhysicalManager* InstanceBuilder::create_physical_instance(
                    RegionTreeForest *forest, LayoutConstraintKind *unsat_kind,
                    unsigned *unsat_index, size_t *footprint)
    //--------------------------------------------------------------------------
    {
      if (!valid)
        initialize(forest);
      // If there are no fields then we are done
      if (field_sizes.empty())
      {
        REPORT_LEGION_WARNING(LEGION_WARNING_IGNORE_MEMORY_REQUEST,
                        "Ignoring request to create instance in "
                        "memory " IDFMT " with no fields.",
                        memory_manager->memory.id);
        if (footprint != NULL)
          *footprint = 0;
        if (unsat_kind != NULL)
          *unsat_kind = LEGION_FIELD_CONSTRAINT;
        if (unsat_index != NULL)
          *unsat_index = 0;
        return NULL;
      }
      if (realm_layout == NULL)
      {
        const std::vector<FieldID> &field_set = 
          constraints.field_constraint.get_field_set();
        realm_layout =
          instance_domain->create_layout(constraints, field_set, field_sizes);
#ifdef DEBUG_LEGION
        assert(realm_layout != NULL);
#endif
      }
      // Clone the realm layout each time since (realm will take ownership 
      // after every instance call, so we need a new one each time)
      Realm::InstanceLayoutGeneric *inst_layout = realm_layout->clone();
#ifdef DEBUG_LEGION
      assert(inst_layout != NULL);
#endif
      // Have to grab this now since realm is going to take ownership of
      // the instance layout generic object once we do the creation call
      const size_t instance_footprint = inst_layout->bytes_used;
      // Save the footprint size if we need to
      if (footprint != NULL)
        *footprint = instance_footprint;
      Realm::ProfilingRequestSet requests;
      // Add a profiling request to see if the instance is actually allocated
      // Make it very high priority so we get the response quickly
      ProfilingResponseBase base(this);
#ifndef LEGION_MALLOC_INSTANCES
      Realm::ProfilingRequest &req = requests.add_request(
          runtime->find_utility_group(), LG_LEGION_PROFILING_ID,
          &base, sizeof(base), LG_RESOURCE_PRIORITY);
      req.add_measurement<Realm::ProfilingMeasurements::InstanceAllocResult>();
      // Create a user event to wait on for the result of the profiling response
      profiling_ready = Runtime::create_rt_user_event();
#endif
#ifdef DEBUG_LEGION
      assert(!instance.exists()); // shouldn't exist before this
#endif
      ApEvent ready;
#ifndef LEGION_MALLOC_INSTANCES
      if (runtime->profiler != NULL)
      {
        runtime->profiler->add_inst_request(requests, creator_id);
        ready = ApEvent(PhysicalInstance::create_instance(instance,
                  memory_manager->memory, inst_layout, requests));
        if (instance.exists())
        {
          unsigned long long creation_time = 
            Realm::Clock::current_time_in_nanoseconds();
          runtime->profiler->record_instance_creation(instance,
              memory_manager->memory, creator_id, creation_time);
        }
      }
      else
        ready = ApEvent(PhysicalInstance::create_instance(instance,
                  memory_manager->memory, inst_layout, requests));
      // Wait for the profiling response
      if (!profiling_ready.has_triggered())
        profiling_ready.wait();
#else
      uintptr_t base_ptr = 0;
      if (instance_footprint > 0)
      {
        base_ptr = 
          memory_manager->allocate_legion_instance(instance_footprint);
        if (base_ptr == 0)
        {
          if (unsat_kind != NULL)
            *unsat_kind = LEGION_MEMORY_CONSTRAINT;
          if (unsat_index != NULL)
            *unsat_index = 0;
          return NULL;
        }
      }
      ready = ApEvent(PhysicalInstance::create_external(instance,
            memory_manager->memory, base_ptr, inst_layout, requests));
#endif
      // If we couldn't make it then we are done
      if (!instance.exists())
      {
        if (unsat_kind != NULL)
          *unsat_kind = LEGION_MEMORY_CONSTRAINT;
        if (unsat_index != NULL)
          *unsat_index = 0;
        return NULL;
      }
      // For Legion Spy we need a unique ready event if it doesn't already
      // exist so we can uniquely identify the instance
      if (!ready.exists() && runtime->legion_spy_enabled)
      {
        ApUserEvent rename_ready = Runtime::create_ap_user_event(NULL);
        Runtime::trigger_event(NULL, rename_ready);
        ready = rename_ready;
      }
      // If we successfully made the instance then Realm 
      // took over ownership of the layout
      PhysicalManager *result = NULL;
      DistributedID did = forest->runtime->get_available_distributed_id();
      AddressSpaceID local_space = forest->runtime->address_space;
      // Important implementation detail here: we pull the pointer constraint
      // out of the set of constraints here and don't include it in the layout
      // constraints so we can abstract over lots of different layouts. We'll
      // store the pointer constraint separately in the physical instance
      PointerConstraint pointer_constraint = constraints.pointer_constraint;
      constraints.pointer_constraint = PointerConstraint();
      // If we successfully made it then we can 
      // switch over the polarity of our constraints, this
      // shouldn't be necessary once Realm gets its act together
      // and actually tells us what the resulting constraints are
      constraints.field_constraint.contiguous = true;
      constraints.field_constraint.inorder = true;
      constraints.ordering_constraint.contiguous = true;
      constraints.memory_constraint = MemoryConstraint(
                                        memory_manager->memory.kind());
      const unsigned num_dims = instance_domain->get_num_dims();
      // Now let's find the layout constraints to use for this instance
      LayoutDescription *layout = field_space_node->find_layout_description(
                                        instance_mask, num_dims, constraints);
      // If we couldn't find one then we make one
      if (layout == NULL)
      {
        // First make a new layout constraint
        LayoutConstraints *layout_constraints = 
          forest->runtime->register_layout(field_space_node->handle,
                                           constraints, true/*internal*/);
        // Then make our description
        layout = field_space_node->create_layout_description(instance_mask, 
                                  num_dims, layout_constraints, mask_index_map,
                                  constraints.field_constraint.get_field_set(),
                                  field_sizes, serdez);
      }
      // Figure out what kind of instance we just made
      switch (constraints.specialized_constraint.get_kind())
      {
        case LEGION_NO_SPECIALIZE:
        case LEGION_AFFINE_SPECIALIZE:
          {
            // Now we can make the manager
            result = new InstanceManager(forest, did, local_space,
                                         memory_manager,
                                         instance, instance_domain, 
                                         field_space_node, tree_id,
                                         layout, pointer_constraint, 
                                         true/*register now*/, 
                                         instance_footprint, ready,
                                         false/*external instance*/);
            break;
          }
        case LEGION_AFFINE_REDUCTION_SPECIALIZE:
          {
            // TODO: this can go away once realm understands reduction
            // instances that contain multiple fields, Legion is ready
            // though so all you should have to do is delete this check
            if (field_sizes.size() > 1)
              REPORT_LEGION_ERROR(ERROR_ILLEGAL_REDUCTION_REQUEST,
                            "Illegal request for a reduction instance "
                            "containing multiple fields. Only a single field "
                            "is currently permitted for reduction instances.")
            ApUserEvent filled_and_ready = Runtime::create_ap_user_event(NULL);
            result = new FoldReductionManager(forest, did, local_space,
                                              memory_manager, 
                                              instance, layout, 
                                              pointer_constraint, 
                                              instance_domain, 
                                              field_space_node, 
                                              tree_id, redop_id,
                                              reduction_op, filled_and_ready,
                                              instance_footprint,
                                              true/*register now*/);
            // Before we can actually use this instance, we have to 
            // initialize it with a fill operation of the proper value
            // Don't record this fill operation because it is just part
            // of the semantics of reduction instances and not something
            // that we want Legion Spy to see
            const PhysicalTraceInfo fake_info(NULL, -1U, false);
            if (!instance_domain->is_empty())
            {
              void *fill_buffer = malloc(reduction_op->sizeof_rhs);
              reduction_op->init(fill_buffer, 1);
              std::vector<CopySrcDstField> dsts;
              {
                const std::vector<FieldID> &fill_fields = 
                  constraints.field_constraint.get_field_set();
                layout->compute_copy_offsets(fill_fields, result, dsts);
              }
#ifdef LEGION_SPY
              ApEvent filled = instance_domain->issue_fill(fake_info, dsts,
                    fill_buffer, reduction_op->sizeof_rhs, 0/*uid*/, 
                    field_space_node->handle, tree_id, ready,
                    PredEvent::NO_PRED_EVENT, NULL, NULL);
#else
              ApEvent filled = instance_domain->issue_fill(fake_info, dsts,
                    fill_buffer, reduction_op->sizeof_rhs, ready,
                    PredEvent::NO_PRED_EVENT, NULL, NULL);
#endif
              // We can free the buffer after we've issued the fill
              free(fill_buffer);
              // Trigger our filled_and_ready event
              Runtime::trigger_event(&fake_info, filled_and_ready, filled);
            }
            else
              Runtime::trigger_event(&fake_info, filled_and_ready);
            break;
          }
        case LEGION_COMPACT_REDUCTION_SPECIALIZE:
          {
            // TODO: implement this
            assert(false);
            break;
          }
        default:
          assert(false); // illegal specialized case
      }
#ifdef LEGION_MALLOC_INSTANCES
      memory_manager->record_legion_instance(result, base_ptr); 
#endif
#ifdef DEBUG_LEGION
      assert(result != NULL);
#endif
      if (runtime->profiler != NULL)
      {
        // Log the logical regions and fields that make up this instance
        for (std::vector<LogicalRegion>::const_iterator it =
              regions.begin(); it != regions.end(); it++)
          runtime->profiler->record_physical_instance_region(creator_id, 
                                                      instance.id, *it);
        runtime->profiler->record_physical_instance_layout(
                                                     creator_id,
                                                     instance.id,
                                                     layout->owner->handle,
                                                     layout->constraints);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::handle_profiling_response(
                                       const ProfilingResponseBase *base,
                                       const Realm::ProfilingResponse &response,
                                       const void *orig, size_t orig_length)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(response.has_measurement<
          Realm::ProfilingMeasurements::InstanceAllocResult>());
#endif
      Realm::ProfilingMeasurements::InstanceAllocResult result;
      result.success = false; // Need this to avoid compiler warnings
#ifdef DEBUG_LEGION
#ifndef NDEBUG
      const bool measured =  
#endif
#endif
        response.get_measurement<
              Realm::ProfilingMeasurements::InstanceAllocResult>(result);
#ifdef DEBUG_LEGION
      assert(measured);
#endif
      // If we failed then clear the instance name since it is not valid
      if (!result.success)
      {
        // Destroy the instance first so that Realm can reclaim the ID
        instance.destroy();
        instance = PhysicalInstance::NO_INST;
      }
      // No matter what trigger the event
      Runtime::trigger_event(profiling_ready);
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::initialize(RegionTreeForest *forest)
    //--------------------------------------------------------------------------
    {
      compute_space_and_domain(forest); 
      compute_layout_parameters();
      valid = true;
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::compute_space_and_domain(RegionTreeForest *forest)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!regions.empty());
      assert(field_space_node == NULL);
      assert(instance_domain == NULL);
      assert(tree_id == 0);
#endif
      std::set<IndexSpaceExpression*> region_exprs;
      for (std::vector<LogicalRegion>::const_iterator it = 
            regions.begin(); it != regions.end(); it++)
      {
        RegionNode *node = forest->get_node(*it);
        if (field_space_node == NULL)
          field_space_node = node->column_source;
        if (tree_id == 0)
          tree_id = it->get_tree_id();
#ifdef DEBUG_LEGION
        // Check to make sure that all the field spaces have the same handle
        assert(field_space_node->handle == it->get_field_space());
        assert(tree_id == it->get_tree_id());
#endif
        region_exprs.insert(node->row_source);
      }
      instance_domain = (region_exprs.size() == 1) ? 
        *(region_exprs.begin()) : forest->union_index_spaces(region_exprs);
      // This also serves to guarantee that the instance domain is
      // valid on the local node
      instance_volume = instance_domain->get_volume();
    }

    //--------------------------------------------------------------------------
    void InstanceBuilder::compute_layout_parameters(void)
    //--------------------------------------------------------------------------
    {
      // First look at the OrderingConstraint to Figure out what kind
      // of instance we are building here, SOA, AOS, or hybrid
      // Make sure to check for splitting constraints if see sub-dimensions
      if (!constraints.splitting_constraints.empty())
        REPORT_LEGION_FATAL(ERROR_UNSUPPORTED_LAYOUT_CONSTRAINT,
            "Splitting layout constraints are not currently supported")
      const size_t num_dims = instance_domain->get_num_dims();
      OrderingConstraint &ord = constraints.ordering_constraint;
      if (!ord.ordering.empty())
      {
        // Find the index of the fields, if it is specified
        int field_idx = -1;
        std::set<DimensionKind> spatial_dims, to_remove;
        for (unsigned idx = 0; idx < ord.ordering.size(); idx++)
        {
          if (ord.ordering[idx] == LEGION_DIM_F)
          {
            // Should never be duplicated 
            if (field_idx != -1)
              REPORT_LEGION_ERROR(ERROR_ILLEGAL_LAYOUT_CONSTRAINT,
                  "Illegal ordering constraint used during instance "
                  "creation contained multiple instances of DIM_F")
            else
              field_idx = idx;
          }
          else if (ord.ordering[idx] > LEGION_DIM_F)
            REPORT_LEGION_FATAL(ERROR_UNSUPPORTED_LAYOUT_CONSTRAINT,
              "Splitting layout constraints are not currently supported")
          else
          {
            // Should never be duplicated
            if (spatial_dims.find(ord.ordering[idx]) != spatial_dims.end())
              REPORT_LEGION_ERROR(ERROR_ILLEGAL_LAYOUT_CONSTRAINT,
                  "Illegal ordering constraint used during instance "
                  "creation contained multiple instances of dimension %d",
                  ord.ordering[idx])
            else
            {
              // Check to make sure that it is one of our dims
              // if not we can just filter it out of the ordering
              if (ord.ordering[idx] >= num_dims)
                to_remove.insert(ord.ordering[idx]);
              else
                spatial_dims.insert(ord.ordering[idx]);
            }
          }
        }
        // Remove any dimensions which don't matter
        if (!to_remove.empty())
        {
          for (std::vector<DimensionKind>::iterator it = ord.ordering.begin();
                it != ord.ordering.end(); /*nothing*/)
          {
            if (to_remove.find(*it) != to_remove.end())
              it = ord.ordering.erase(it);
            else
              it++;
          }
        }
#ifdef DEBUG_LEGION
        assert(spatial_dims.size() <= num_dims);
#endif
        // Fill in any spatial dimensions that we didn't see if necessary
        if (spatial_dims.size() < num_dims)
        {
          // See if we should push these dims front or back
          if (field_idx > -1)
          {
            // See if we should add these at the front or the back
            if (field_idx == 0)
            {
              // Add them to the back
              for (unsigned idx = 0; idx < num_dims; idx++)
              {
                DimensionKind dim = (DimensionKind)(LEGION_DIM_X + idx);
                if (spatial_dims.find(dim) == spatial_dims.end())
                  ord.ordering.push_back(dim);
              }
            }
            else if (field_idx == int(ord.ordering.size()-1))
            {
              // Add them to the front
              for (int idx = (num_dims-1); idx >= 0; idx--)
              {
                DimensionKind dim = (DimensionKind)(LEGION_DIM_X + idx);
                if (spatial_dims.find(dim) == spatial_dims.end())
                  ord.ordering.insert(ord.ordering.begin(), dim);
              }
            }
            else // Should either be AOS or SOA for now
              assert(false);
          }
          else
          {
            // No field dimension so just add the spatial ones on the back
            for (unsigned idx = 0; idx < num_dims; idx++)
            {
              DimensionKind dim = (DimensionKind)(LEGION_DIM_X + idx);
              if (spatial_dims.find(dim) == spatial_dims.end())
                ord.ordering.push_back(dim);
            }
          }
        }
        // If we didn't see the field dimension either then add that
        // at the end to give us SOA layouts in general
        if (field_idx == -1)
          ord.ordering.push_back(LEGION_DIM_F);
        // We've now got all our dimensions so we can set the
        // contiguous flag to true
        ord.contiguous = true;
      }
      else
      {
        // We had no ordering constraints so populate it with 
        // SOA constraints for now
        for (unsigned idx = 0; idx < num_dims; idx++)
          ord.ordering.push_back((DimensionKind)(LEGION_DIM_X + idx));
        ord.ordering.push_back(LEGION_DIM_F);
        ord.contiguous = true;
      }
#ifdef DEBUG_LEGION
      assert(ord.contiguous);
      assert(ord.ordering.size() == (num_dims + 1));
#endif
      // From this we should be able to compute the field groups 
      // Use the FieldConstraint to put any fields in the proper order
      const std::vector<FieldID> &field_set = 
        constraints.field_constraint.get_field_set(); 
      field_sizes.resize(field_set.size());
      mask_index_map.resize(field_set.size());
      serdez.resize(field_set.size());
      field_space_node->compute_field_layout(field_set, field_sizes,
                                       mask_index_map, serdez, instance_mask);
      // See if we have any specialization here that will 
      // require us to update the field sizes
      switch (constraints.specialized_constraint.get_kind())
      {
        case LEGION_NO_SPECIALIZE:
        case LEGION_AFFINE_SPECIALIZE:
          break;
        case LEGION_AFFINE_REDUCTION_SPECIALIZE:
          {
            // Reduction folds are a special case of normal specialize
            redop_id = constraints.specialized_constraint.get_reduction_op();
            reduction_op = Runtime::get_reduction_op(redop_id);
            for (unsigned idx = 0; idx < field_sizes.size(); idx++)
            {
              if (field_sizes[idx] != reduction_op->sizeof_lhs)
                REPORT_LEGION_ERROR(ERROR_UNSUPPORTED_LAYOUT_CONSTRAINT,
                    "Illegal reduction instance request with field %d "
                    "which has size %d but the LHS type of reduction "
                    "operator %d is %d", field_set[idx], int(field_sizes[idx]),
                    redop_id, int(reduction_op->sizeof_lhs))
              // Update the field sizes to the rhs of the reduction op
              field_sizes[idx] = reduction_op->sizeof_rhs;
            }
            break;
          }
        case LEGION_COMPACT_REDUCTION_SPECIALIZE:
          {
            // TODO: implement list reduction instances
            assert(false);
            redop_id = constraints.specialized_constraint.get_reduction_op();
            reduction_op = Runtime::get_reduction_op(redop_id);
            break;
          }
        case LEGION_VIRTUAL_SPECIALIZE:
          {
            REPORT_LEGION_ERROR(ERROR_ILLEGAL_REQUEST_VIRTUAL_INSTANCE,
                          "Illegal request to create a virtual instance");
            assert(false);
          }
        default:
          assert(false); // unknown kind
      }
    }

  }; // namespace Internal
}; // namespace Legion

