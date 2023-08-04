#include <exception>

#include <jank/read/lex.hpp>
#include <jank/read/parse.hpp>
#include <jank/runtime/context.hpp>
#include <jank/runtime/obj/native_function_wrapper.hpp>
#include <jank/runtime/obj/string.hpp>
#include <jank/runtime/obj/number.hpp>
#include <jank/runtime/util.hpp>
#include <jank/runtime/seq.hpp>
#include <jank/analyze/processor.hpp>
#include <jank/codegen/processor.hpp>
#include <jank/evaluate.hpp>
#include <jank/jit/processor.hpp>
#include <jank/util/mapped_file.hpp>
#include <jank/util/process_location.hpp>

namespace jank::runtime
{
  context::context()
  {
    auto &t_state(get_thread_state());
    auto const core(intern_ns(jank::make_box<obj::symbol>("clojure.core")));
    {
      auto const locked_core_vars(core->vars.wlock());
      auto const ns_sym(jank::make_box<obj::symbol>("clojure.core/*ns*"));
      auto const ns_res(locked_core_vars->insert({ns_sym, jank::make_box<var>(core, ns_sym, core)}));
      t_state.current_ns = ns_res.first->second;
    }

    auto const in_ns_sym(jank::make_box<obj::symbol>("clojure.core/in-ns"));
    std::function<object_ptr (object_ptr)> in_ns_fn
    (
      [this](object_ptr const sym)
      {
        return visit_object
        (
          [this](auto const typed_sym)
          {
            using T = typename decltype(typed_sym)::value_type;

            if constexpr(std::same_as<T, obj::symbol>)
            {
              auto const new_ns(intern_ns(typed_sym));
              get_thread_state().current_ns->set_root(new_ns);
              return obj::nil::nil_const();
            }
            else
            /* TODO: throw. */
            { return obj::nil::nil_const(); }
          },
          sym
        );
      }
   );
    auto in_ns_var(intern_var(in_ns_sym).expect_ok());
    in_ns_var->set_root(make_box<obj::native_function_wrapper>(in_ns_fn));
    t_state.in_ns = in_ns_var;

    /* TODO: Remove this once it can be defined in jank. */
    auto const assert_sym(jank::make_box<obj::symbol>("clojure.core/assert"));
    std::function<object_ptr (object_ptr)> assert_fn
    (
      [](object_ptr const o)
      {
        if(!o || !detail::truthy(o))
        { throw std::runtime_error{ "assertion failed" }; }
        return obj::nil::nil_const();
      }
    );
    intern_var(assert_sym).expect_ok()->set_root(make_box<obj::native_function_wrapper>(assert_fn));

    /* TODO: Remove this once it can be defined in jank. */
    auto const seq_sym(jank::make_box<obj::symbol>("clojure.core/seq"));
    intern_var(seq_sym).expect_ok()->set_root(make_box<obj::native_function_wrapper>(static_cast<object_ptr (*)(object_ptr)>(&seq)));

    auto const fresh_seq_sym(jank::make_box<obj::symbol>("clojure.core/fresh-seq"));
    intern_var(fresh_seq_sym).expect_ok()->set_root(make_box<obj::native_function_wrapper>(&fresh_seq));
  }

  context::context(context const &ctx)
  {
    auto ns_lock(namespaces.wlock());
    for(auto const &ns : *ctx.namespaces.rlock())
    { ns_lock->insert({ ns.first, ns.second->clone() }); }
    *keywords.wlock() = *ctx.keywords.rlock();
    *thread_states.wlock() = *ctx.thread_states.rlock();
  }

  obj::symbol_ptr context::qualify_symbol(obj::symbol_ptr const &sym)
  {
    obj::symbol_ptr qualified_sym{ sym };
    if(qualified_sym->ns.empty())
    {
      auto const t_state(get_thread_state());
      auto const current_ns(expect_object<ns>(t_state.current_ns->get_root()));
      qualified_sym = jank::make_box<obj::symbol>(current_ns->name->name, sym->name);
    }
    return qualified_sym;
  }

  option<var_ptr> context::find_var(obj::symbol_ptr const &sym)
  {
    if(!sym->ns.empty())
    {
      /* TODO: This is the issue. Diff it with intern_var. */
      ns_ptr ns{};
      {
        auto const locked_namespaces(namespaces.rlock());
        auto const found(locked_namespaces->find(jank::make_box<obj::symbol>("", sym->ns)));
        if(found == locked_namespaces->end())
        { return none; }
        ns = found->second;
      }

      {
        auto const locked_vars(ns->vars.rlock());
        auto const found(locked_vars->find(sym));
        if(found == locked_vars->end())
        { return none; }

        return { found->second };
      }
    }
    else
    {
      auto const t_state(get_thread_state());
      auto const current_ns(expect_object<ns>(t_state.current_ns->get_root()));
      auto const locked_vars(current_ns->vars.rlock());
      auto const qualified_sym(jank::make_box<obj::symbol>(current_ns->name->name, sym->name));
      auto const found(locked_vars->find(qualified_sym));
      if(found == locked_vars->end())
      { return none; }

      return { found->second };
    }
  }

  option<object_ptr> context::find_local(obj::symbol_ptr const &)
  {
    return none;
  }

  void context::eval_prelude(jit::processor const &jit_prc)
  {
    auto const jank_path(jank::util::process_location().unwrap().parent_path());
    auto const src_path(jank_path / "../src/jank/clojure/core.jank");
    eval_file(src_path.string(), jit_prc);
  }

  object_ptr context::eval_file(native_string_view const &path, jit::processor const &jit_prc)
  {
    auto const file(util::map_file(path));
    if(file.is_err())
    { throw std::runtime_error{ fmt::format("unable to map file {} due to error: {}", path, file.expect_err()) }; }
    return eval_string({ file.expect_ok().head, file.expect_ok().size }, jit_prc);
  }

  object_ptr context::eval_string(native_string_view const &code, jit::processor const &jit_prc)
  {
    read::lex::processor l_prc{ code };
    read::parse::processor p_prc{ *this, l_prc.begin(), l_prc.end() };
    jank::analyze::processor an_prc{ *this };

    object_ptr ret{};
    for(auto const &form : p_prc)
    {
      auto const expr(an_prc.analyze(form.expect_ok(), analyze::expression_type::statement));
      ret = evaluate::eval(*this, jit_prc, expr.expect_ok());
    }

    return ret;
  }

  native_string context::unique_string()
  { return unique_string("gen"); }
  native_string context::unique_string(native_string_view const &prefix)
  {
    static std::atomic_size_t index{ 1 };
    return prefix.data() + std::to_string(index++);
  }
  obj::symbol context::unique_symbol()
  { return unique_symbol("gen"); }
  obj::symbol context::unique_symbol(native_string_view const &prefix)
  { return { "", unique_string(prefix) }; }

  void context::dump() const
  {
    std::cout << "context dump" << std::endl;
    auto locked_namespaces(namespaces.rlock());
    for(auto const &p : *locked_namespaces)
    {
      std::cout << "  " << p.second->name->to_string() << std::endl;
      auto locked_vars(p.second->vars.rlock());
      for(auto const &vp : *locked_vars)
      {
        if(vp.second->get_root() == nullptr)
        { std::cout << "    " << vp.second->to_string() << " = nil" << std::endl; }
        else
        { std::cout << "    " << vp.second->to_string() << " = " << detail::to_string(vp.second->get_root()) << std::endl; }
      }
    }
  }

  ns_ptr context::intern_ns(obj::symbol_ptr const &sym)
  {
    auto locked_namespaces(namespaces.wlock());
    auto const found(locked_namespaces->find(sym));
    if(found != locked_namespaces->end())
    { return found->second; }

    auto const result(locked_namespaces->emplace(sym, jank::make_box<ns>(sym, *this)));
    return result.first->second;
  }

  result<var_ptr, native_string> context::intern_var
  (native_string const &ns, native_string const &name)
  { return intern_var(jank::make_box<obj::symbol>(ns, name)); }
  result<var_ptr, native_string> context::intern_var(obj::symbol_ptr const &qualified_sym)
  {
    if(qualified_sym->ns.empty())
    { return err("can't intern var; sym isn't qualified"); }

    auto locked_namespaces(namespaces.wlock());
    auto const found_ns(locked_namespaces->find(jank::make_box<obj::symbol>(qualified_sym->ns)));
    if(found_ns == locked_namespaces->end())
    { return err("can't intern var; namespace doesn't exist"); }

    /* TODO: Read lock, then upgrade as needed? Benchmark. */
    auto locked_vars(found_ns->second->vars.wlock());
    auto const found_var(locked_vars->find(qualified_sym));
    if(found_var != locked_vars->end())
    { return ok(found_var->second); }

    auto const ns_res
    (locked_vars->insert({qualified_sym, jank::make_box<var>(found_ns->second, qualified_sym)}));
    return ok(ns_res.first->second);
  }

  /* TODO: Swap these. The other one makes a symbol anyway. */
  obj::keyword_ptr context::intern_keyword(obj::symbol const &sym, bool const resolved)
  { return intern_keyword(sym.ns, sym.name, resolved); }
  obj::keyword_ptr context::intern_keyword
  (native_string_view const &ns, native_string_view const &name, bool resolved)
  {
    obj::symbol sym{ ns, name };
    if(!resolved)
    {
      /* The ns will be an ns alias. */
      if(!ns.empty())
      { throw std::runtime_error{ "unimplemented: auto-resolved ns aliases" }; }
      else
      {
        auto const t_state(get_thread_state());
        auto const current_ns(expect_object<jank::runtime::ns>(t_state.current_ns->get_root()));
        sym.ns = current_ns->name->name;
        resolved = true;
      }
    }

    auto locked_keywords(keywords.wlock());
    auto const found(locked_keywords->find(sym));
    if(found != locked_keywords->end())
    { return found->second; }

    auto const res(locked_keywords->emplace(sym, make_box<obj::keyword>(sym, resolved)));
    return res.first->second;
  }

  object_ptr context::macroexpand1(object_ptr o)
  {
    return visit_object
    (
      [this](auto const typed_o) -> object_ptr
      {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(!std::same_as<T, obj::list>)
        { return typed_o; }
        else
        {
          if(typed_o->data.data->length == 0)
          { return typed_o; }

          auto const first_sym_obj(typed_o->data.first().unwrap());
          if(first_sym_obj->type != object_type::symbol)
          { return typed_o; }

          auto const var(find_var(expect_object<obj::symbol>(first_sym_obj)));
          /* None means it's not a var, so not a macro. No meta means no :macro set. */
          if(var.is_none() || var.unwrap()->meta.is_none())
          { return typed_o; }

          auto const meta(var.unwrap()->meta.unwrap());
          auto const found_macro(meta->data.find(intern_keyword("", "macro", true)));
          if(!found_macro || !detail::truthy(found_macro))
          { return typed_o; }

          auto const &args(jank::make_box<obj::list>(typed_o->data.rest().cons(obj::nil::nil_const()).cons(typed_o)));
          return apply_to(var.unwrap()->get_root(), args);
        }
      },
      o
    );
  }

  object_ptr context::macroexpand(object_ptr o)
  {
    auto const expanded(macroexpand1(o));
    if(expanded != o)
    { return macroexpand(expanded); }
    return o;
  }

  object_ptr context::print(object_ptr const o)
  {
    auto const s(detail::to_string(o));
    std::fwrite(s.data(), 1, s.size(), stdout);
    return obj::nil::nil_const();
  }

  object_ptr context::print(object_ptr const o, object_ptr const more)
  {
    visit_object
    (
      [o](auto const typed_more)
      {
        using T = typename decltype(typed_more)::value_type;

        if constexpr(behavior::sequenceable<T>)
        {
          fmt::memory_buffer buff;
          auto inserter(std::back_inserter(buff));
          detail::to_string(o, buff);
          detail::to_string(typed_more->first(), buff);
          for(auto it(typed_more->next_in_place()); it != nullptr; it = it->next_in_place())
          {
            fmt::format_to(inserter, " ");
            detail::to_string(it->first(), buff);
          }
          std::fwrite(buff.data(), 1, buff.size(), stdout);
        }
        else
        { throw std::runtime_error{ fmt::format("expected a sequence: {}", typed_more->to_string()) }; }
      },
      more
    );
    return obj::nil::nil_const();
  }

  object_ptr context::println(object_ptr const more)
  {
    visit_object
    (
      [](auto const typed_more)
      {
        using T = typename decltype(typed_more)::value_type;

        if constexpr(behavior::sequenceable<T>)
        {
          fmt::memory_buffer buff;
          auto inserter(std::back_inserter(buff));
          detail::to_string(typed_more->first(), buff);
          for(auto it(typed_more->next_in_place()); it != nullptr; it = it->next_in_place())
          {
            fmt::format_to(inserter, " ");
            detail::to_string(it->first(), buff);
          }
          std::fwrite(buff.data(), 1, buff.size(), stdout);
          std::putc('\n', stdout);
        }
        else
        { throw std::runtime_error{ fmt::format("expected a sequence: {}", typed_more->to_string()) }; }
      },
      more
    );
    return obj::nil::nil_const();
  }

  context::thread_state::thread_state(context &ctx)
    : rt_ctx{ ctx }
  { }

  context::thread_state& context::get_thread_state()
  { return get_thread_state(none); }
  context::thread_state& context::get_thread_state(option<thread_state> init)
  {
    auto const this_id(std::this_thread::get_id());
    decltype(thread_states)::DataType::iterator found;

    /* TODO: Can this just use an upgrade lock? */
    /* Assume it's there and use a read lock. */
    {
      auto const locked_thread_states(thread_states.rlock());
      /* Our read lock here is on the container; we're returning a mutable item, but
         that's because the item itself is thread-local. */
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      found = const_cast
      <native_unordered_map<std::thread::id, thread_state>&>(*locked_thread_states).find(this_id);
      if(found != locked_thread_states->end())
      { return found->second; }
    }

    /* If it's not there, use a write lock and put it there (but check again first). */
    {
      auto const locked_thread_states(thread_states.wlock());
      found = locked_thread_states->find(this_id);
      if(found != locked_thread_states->end())
      { return found->second; }
      else if(init.is_some())
      { found = locked_thread_states->emplace(this_id, std::move(init.unwrap())).first; }
      else
      { found = locked_thread_states->emplace(this_id, thread_state{ *this }).first; }
      return found->second;
    }
  }
}
