/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReachableClasses.h"

#include <chrono>
#include <string>
#include <unordered_set>
#include <fstream>
#include <string>

#include "walkers.h"
#include "DexClass.h"
#include "Predicate.h"
#include "RedexResources.h"

namespace {

// Note: this method will return nullptr if the dotname refers to an unknown
// type.
DexType* get_dextype_from_dotname(const char* dotname) {
  if (dotname == nullptr) {
    return nullptr;
  }
  std::string buf;
  buf.reserve(strlen(dotname) + 2);
  buf += 'L';
  buf += dotname;
  buf += ';';
  std::replace(buf.begin(), buf.end(), '.', '/');
  return DexType::get_type(buf.c_str());
}

/**
 * Class is used directly in code (As opposed to used via reflection)
 *
 * For example, it could be used by one of these instructions:
 *   check-cast
 *   new-instance
 *   const-class
 *   instance-of
 */
void mark_reachable_directly(DexClass* dclass) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_type();
  // When we mark a class as reachable, we also mark all fields and methods as
  // reachable.  Eventually we will be smarter about this, which will allow us
  // to remove unused methods and fields.
  for (DexMethod* dmethod : dclass->get_dmethods()) {
    dmethod->rstate.ref_by_type();
  }
  for (DexMethod* vmethod : dclass->get_vmethods()) {
    vmethod->rstate.ref_by_type();
  }
  for (DexField* sfield : dclass->get_sfields()) {
    sfield->rstate.ref_by_type();
  }
  for (DexField* ifield : dclass->get_ifields()) {
    ifield->rstate.ref_by_type();
  }
}

template<typename DexMember>
void mark_only_reachable_directly(DexMember* m) {
   m->rstate.ref_by_type();
}

/**
 * Indicates that a class is being used via reflection.
 *
 * If from_code is true, it's used from the dex files, otherwise it is
 * used by an XML file or from native code.
 *
 * Examples:
 *
 *   Bar.java: (from_code = true, directly created via reflection)
 *     Object x = Class.forName("com.facebook.Foo").newInstance();
 *
 *   MyGreatLayout.xml: (from_code = false, created when view is inflated)
 *     <com.facebook.MyTerrificView />
 */
void mark_reachable_by_classname(DexClass* dclass, bool from_code) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_string(from_code);
  // When we mark a class as reachable, we also mark all fields and methods as
  // reachable.  Eventually we will be smarter about this, which will allow us
  // to remove unused methods and fields.
  for (DexMethod* dmethod : dclass->get_dmethods()) {
    dmethod->rstate.ref_by_string(from_code);
  }
  for (DexMethod* vmethod : dclass->get_vmethods()) {
    vmethod->rstate.ref_by_string(from_code);
  }
  for (DexField* sfield : dclass->get_sfields()) {
    sfield->rstate.ref_by_string(from_code);
  }
  for (DexField* ifield : dclass->get_ifields()) {
    ifield->rstate.ref_by_string(from_code);
  }
}

void mark_reachable_by_classname(DexType* dtype, bool from_code) {
  mark_reachable_by_classname(type_class_internal(dtype), from_code);
}

void mark_reachable_by_classname(std::string& classname, bool from_code) {
  DexString* dstring =
      DexString::get_string(classname.c_str(), (uint32_t)classname.size());
  DexType* dtype = DexType::get_type(dstring);
  if (dtype == nullptr) return;
  DexClass* dclass = type_class_internal(dtype);
  mark_reachable_by_classname(dclass, from_code);
}

void mark_reachable_by_seed(DexClass* dclass) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_seed();
}

void mark_reachable_by_seed(DexType* dtype) {
  if (dtype == nullptr) return;
  mark_reachable_by_seed(type_class_internal(dtype));
}

template <typename DexMember>
bool anno_set_contains(
  DexMember m,
  const std::unordered_set<DexType*>& keep_annotations
) {
  auto const& anno_set = m->get_anno_set();
  if (anno_set == nullptr) return false;
  auto const& annos = anno_set->get_annotations();
  for (auto const& anno : annos) {
    if (keep_annotations.count(anno->type())) {
      return true;
    }
  }
  return false;
}

void keep_annotated_classes(
  const Scope& scope,
  const std::unordered_set<DexType*>& keep_annotations
) {
  for (auto const& cls : scope) {
    if (anno_set_contains(cls, keep_annotations)) {
      mark_only_reachable_directly(cls);
    }
    for (auto const& m : cls->get_dmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_sfields()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_ifields()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
  }
}

/*
 * This method handles the keep_class_members from the configuration file.
 */
void keep_class_members(
    const Scope& scope,
    const std::vector<std::string>& keep_class_mems) {
  for (auto const& cls : scope) {
    std::string name = std::string(cls->get_type()->get_name()->c_str());
    for (auto const& class_mem : keep_class_mems) {
      std::string class_mem_str = std::string(class_mem.c_str());
      std::size_t pos = class_mem_str.find(name);
      if (pos != std::string::npos) {
        std::string rem_str = class_mem_str.substr(pos+name.size());
        for (auto const& f : cls->get_sfields()) {
          if (rem_str.find(std::string(f->get_name()->c_str()))!=std::string::npos) {
            mark_only_reachable_directly(f);
            mark_only_reachable_directly(cls);
          }
        }
        break;
      }
    }
  }
}

void keep_methods(const Scope& scope, const std::vector<std::string>& ms) {
  std::set<std::string> methods_to_keep(ms.begin(), ms.end());
  for (auto const& cls : scope) {
    for (auto& m : cls->get_dmethods()) {
      if (methods_to_keep.count(m->get_name()->c_str())) {
        m->rstate.ref_by_string(false);
      }
    }
    for (auto& m : cls->get_vmethods()) {
      if (methods_to_keep.count(m->get_name()->c_str())) {
        m->rstate.ref_by_string(false);
      }
    }
  }
}

/*
 * Returns true iff this class or any of its super classes are in the set of
 * classes banned due to use of complex reflection.
 */
bool in_reflected_pkg(DexClass* dclass,
                      std::unordered_set<DexClass*>& reflected_pkg_classes) {
  if (dclass == nullptr) {
    // Not in our dex files
    return false;
  }

  if (reflected_pkg_classes.count(dclass)) {
    return true;
  }
  return in_reflected_pkg(type_class_internal(dclass->get_super_class()),
                          reflected_pkg_classes);
}

/*
 * Initializes list of classes that are reachable via reflection, and calls
 * or from code.
 *
 * These include:
 *  - Classes used in the manifest (e.g. activities, services, etc)
 *  - View or Fragment classes used in layouts
 *  - Classes that are in certain packages (specified in the reflected_packages
 *    section of the config) and classes that extend from them
 *  - Classes marked with special annotations (keep_annotations in config)
 *  - Classes reachable from native libraries
 */
void init_permanently_reachable_classes(
  const Scope& scope,
  const Json::Value& config,
  const std::vector<KeepRule>& proguard_rules,
  const std::unordered_set<DexType*>& no_optimizations_anno
) {
  PassConfig pc(config);

  std::string apk_dir;
  std::vector<std::string> reflected_package_names;
  std::vector<std::string> annotations;
  std::vector<std::string> class_members;
  std::vector<std::string> methods;

  pc.get("apk_dir", "", apk_dir);
  pc.get("keep_packages", {}, reflected_package_names);
  pc.get("keep_annotations", {}, annotations);
  pc.get("keep_class_members", {}, class_members);
  pc.get("keep_methods", {}, methods);

  std::unordered_set<DexType*> annotation_types(
    no_optimizations_anno.begin(),
    no_optimizations_anno.end());

  for (auto const& annostr : annotations) {
    DexType* anno = DexType::get_type(annostr.c_str());
    if (anno) annotation_types.insert(anno);
  }

  keep_annotated_classes(scope, annotation_types);
  keep_class_members(scope, class_members);
  keep_methods(scope, methods);

  if (apk_dir.size()) {
    // Classes present in manifest
    std::string manifest = apk_dir + std::string("/AndroidManifest.xml");
    for (std::string classname : get_manifest_classes(manifest)) {
      TRACE(PGR, 3, "manifest: %s\n", classname.c_str());
      mark_reachable_by_classname(classname, false);
    }

    // Classes present in XML layouts
    for (std::string classname : get_layout_classes(apk_dir)) {
      TRACE(PGR, 3, "xml_layout: %s\n", classname.c_str());
      mark_reachable_by_classname(classname, false);
    }

    // Classnames present in native libraries (lib/*/*.so)
    for (std::string classname : get_native_classes(apk_dir)) {
      auto type = DexType::get_type(classname.c_str());
      if (type == nullptr) continue;
      TRACE(PGR, 3, "native_lib: %s\n", classname.c_str());
      mark_reachable_by_classname(type, false);
    }
  }

  std::unordered_set<DexClass*> reflected_package_classes;
  for (auto clazz : scope) {
    const char* cname = clazz->get_type()->get_name()->c_str();
    for (auto pkg : reflected_package_names) {
      if (starts_with(cname, pkg.c_str())) {
        reflected_package_classes.insert(clazz);
        continue;
      }
    }
  }
  for (auto clazz : scope) {
    if (in_reflected_pkg(clazz, reflected_package_classes)) {
      reflected_package_classes.insert(clazz);
      /* Note:
       * Some of these are by string, others by type
       * but we have no way in the config to distinguish
       * them currently.  So, we mark with the most
       * conservative sense here.
       */
      TRACE(PGR, 3, "reflected_package: %s\n", SHOW(clazz));
      mark_reachable_by_classname(clazz, false);
    }
  }

  /* Do only keep class rules for now.
   * '*' and '**' rules are skipped,
   * because those are matching on something else,
   * which we haven't implemented yet.
   * Rules can be "*" or "**" on classname and match
   * on some other attribute. We don't match against
   * all attributes at once, so this prevents us
   * from matching everything.
   */
  std::vector<std::string> cls_patterns;
  for (auto const& r : proguard_rules) {
    if (r.classname != nullptr &&
        (r.class_type == keeprules::ClassType::CLASS ||
         r.class_type == keeprules::ClassType::INTERFACE) &&
          strlen(r.classname) > 2) {
      std::string cls_pattern(r.classname);
      std::replace(cls_pattern.begin(), cls_pattern.end(), '.', '/');
      auto prep_pat = 'L' + cls_pattern;
      TRACE(PGR, 2, "adding pattern %s \n", prep_pat.c_str());
      cls_patterns.push_back(prep_pat);
    }
  }
  size_t pg_marked_classes = 0;
  for (auto clazz : scope) {
    auto cname = clazz->get_type()->get_name()->c_str();
    auto cls_len = strlen(cname);
    for (auto const& pat : cls_patterns) {
        size_t pat_len = pat.size();
        if (type_matches(pat.c_str(), cname, pat_len, cls_len)) {
          mark_reachable_directly(clazz);
          TRACE(PGR, 2, "matched cls %s against pattern %s \n",
              cname, pat.c_str());
          pg_marked_classes++;
          break;
      }
    }
  }
  TRACE(PGR, 1, "matched on %lu classes with CLASS KEEP proguard rules \n",
      pg_marked_classes);
}

}

/**
 * Walks all the code of the app, finding classes that are reachable from
 * code.
 *
 * Note that as code is changed or removed by Redex, this information will
 * become stale, so this method should be called periodically, for example
 * after each pass.
 */
void recompute_classes_reachable_from_code(const Scope& scope) {
  // Matches methods marked as native
  walk_methods(scope,
               [&](DexMethod* meth) {
                 if (meth->get_access() & DexAccessFlags::ACC_NATIVE) {
                   TRACE(PGR, 3, "native_method: %s\n", SHOW(meth->get_class()));
                   mark_reachable_by_classname(meth->get_class(), true);
                 }
               });
}

void reportReachableClasses(const Scope& scope, std::string reportFileName) {
  TRACE(PGR, 4, "Total numner of classes: %d\n", scope.size());
  // Report classes that the reflection filter says can't be deleted.
  std::ofstream reportFileCanDelete(reportFileName + ".cant_delete");
  for (auto const& cls : scope) {
    if (!can_delete(cls)) {
      reportFileCanDelete << cls->get_name()->c_str() << "\n";
    }
  }
  reportFileCanDelete.close();
  // Report classes that the reflection filter says can't be renamed.
  std::ofstream reportFileCanRename(reportFileName + ".cant_rename");
  for (auto const& cls : scope) {
    if (!can_rename(cls)) {
      reportFileCanRename << cls->get_name()->c_str() << "\n";
    }
  }
  reportFileCanRename.close();
  // Report classes marked for keep from ProGuard flat file list.
  std::ofstream reportFileKeep(reportFileName + ".must_keep");
  for (auto const& cls : scope) {
    if (is_seed(cls)) {
      reportFileKeep << cls->get_name()->c_str() << "\n";
    }
  }
  reportFileKeep.close();
}

void init_reachable_classes(
    const Scope& scope,
    const Json::Value& config,
    const std::vector<KeepRule>& proguard_rules,
    const std::unordered_set<DexType*>& no_optimizations_anno) {
  // Find classes that are reachable in such a way that none of the redex
  // passes will cause them to be no longer reachable.  For example, if a
  // class is referenced from the manifest.
  init_permanently_reachable_classes(
      scope, config, proguard_rules, no_optimizations_anno);

  // Classes that are reachable in ways that could change as Redex runs. For
  // example, a class might be instantiated from a method, but if that method
  // is later deleted then it might no longer be reachable.
  recompute_classes_reachable_from_code(scope);
}

unsigned int init_seed_classes(const std::string seeds_filename) {
    TRACE(PGR, 8, "Reading seed classes from %s\n", seeds_filename.c_str());
    auto start = std::chrono::high_resolution_clock::now();
    std::ifstream seeds_file(seeds_filename);
    unsigned int count = 0;
    if (!seeds_file) {
      TRACE(PGR, 8, "Seeds file %s was not found (ignoring error).",
          seeds_filename.c_str());
			return 0;
    } else {
      std::string line;
      while (getline(seeds_file, line)) {
        if (line.find(":") == std::string::npos && line.find("$") ==
            std::string::npos) {
          auto dex_type = get_dextype_from_dotname(line.c_str());
          if (dex_type != nullptr) {
            mark_reachable_by_seed(dex_type);
            count++;
          } else {
            TRACE(PGR, 2,
                "Seed file contains class for which "
                "Dex type can't be found: %s\n",
                line.c_str());
          }
        }
      }
      seeds_file.close();
    }
    auto end = std::chrono::high_resolution_clock::now();
    TRACE(PGR, 1, "Read %d seed classes in %.1lf seconds\n", count,
          std::chrono::duration<double>(end - start).count());
		return count;
}
