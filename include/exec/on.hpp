/*
 * Copyright (c) 2022 NVIDIA Corporation
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "../stdexec/execution.hpp"
#include "env.hpp"
#include "__detail/__sender_facade.hpp"

namespace exec {
  /////////////////////////////////////////////////////////////////////////////
  // A scoped version of [execution.senders.adaptors.on]
  namespace __on {
    using namespace stdexec;

    enum class on_kind { start_on, continue_on };

    template <on_kind>
      struct on_t;

    template <class _Scheduler, class _Sender>
      struct __start_fn {
        _Scheduler __sched_;
        _Sender __sndr_;

        template <class _Self, class _OldSched>
          static auto __call(_Self&& __self, _OldSched __old_sched) {
            return std::move(((_Self&&) __self).__sndr_)
              | exec::write(exec::with(get_scheduler, __self.__sched_))
              | transfer(__old_sched)
              ;
          }

        template <scheduler _OldSched>
          auto operator()(_OldSched __old_sched) && {
            return __call(std::move(*this), __old_sched);
          }
        template <scheduler _OldSched>
          auto operator()(_OldSched __old_sched) const & {
            return __call(*this, __old_sched);
          }
      };
    template <class _Scheduler, class _Sender>
      __start_fn(_Scheduler, _Sender)
        -> __start_fn<_Scheduler, _Sender>;

    template <class _Env, class _Sender>
      struct _ENVIRONMENT_HAS_NO_SCHEDULER_FOR_THE_ON_ADAPTOR_TO_TRANSITION_BACK_TO {};

    template <class _SchedulerId, class _SenderId>
      struct __start_on_sender {
        using _Scheduler = __t<_SchedulerId>;
        using _Sender = __t<_SenderId>;

        _Scheduler __sched_;
        _Sender __sndr_;

        template <class _Self>
          static auto __call(_Self&& __self) {
            return let_value(
              stdexec::read(get_scheduler)
                | transfer(__self.__sched_),
              __start_fn{__self.__sched_, ((_Self&&) __self).__sndr_});
          }

        template <__decays_to<__start_on_sender> _Self>
          using __inner_t = decltype(__call(__declval<_Self>()));

        template <__decays_to<__start_on_sender> _Self, receiver _Receiver>
            requires constructible_from<_Sender, __member_t<_Self, _Sender>> &&
              sender_to<__inner_t<_Self>, _Receiver>
          friend auto tag_invoke(connect_t, _Self&& __self, _Receiver&& __receiver)
            -> connect_result_t<__inner_t<_Self>, _Receiver> {
            return connect(__call((_Self&&) __self), (_Receiver&&) __receiver);
          }

        template <__decays_to<__start_on_sender> _Self, class _Env>
          friend auto tag_invoke(get_completion_signatures_t, _Self&&, _Env&&)
            -> completion_signatures_of_t<__inner_t<_Self>, _Env>;

        template <__decays_to<__start_on_sender> _Self, __none_of<no_env> _Env>
            requires (!__callable<get_scheduler_t, _Env>)
          friend auto tag_invoke(get_completion_signatures_t, _Self&&, _Env)
            -> _ENVIRONMENT_HAS_NO_SCHEDULER_FOR_THE_ON_ADAPTOR_TO_TRANSITION_BACK_TO<_Env, _Sender>;

        // forward sender queries:
        template <tag_category<forwarding_sender_query> _Tag, class... _As>
            requires __callable<_Tag, const _Sender&, _As...>
          friend auto tag_invoke(_Tag __tag, const __start_on_sender& __self, _As&&... __as)
            noexcept(__nothrow_callable<_Tag, const _Sender&, _As...>)
            -> __call_result_if_t<tag_category<_Tag, forwarding_sender_query>, _Tag, const _Sender&, _As...> {
            return ((_Tag&&) __tag)(__self.__sndr_, (_As&&) __as...);
          }
      };
    template <class _Scheduler, class _Sender>
      __start_on_sender(_Scheduler, _Sender)
        -> __start_on_sender<__x<_Scheduler>, __x<_Sender>>;

    template <>
      struct on_t<on_kind::start_on> {
        template <scheduler _Scheduler, sender _Sender>
            requires constructible_from<decay_t<_Sender>, _Sender>
          auto operator()(_Scheduler&& __sched, _Sender&& __sndr) const {
            // connect-based customization will remove the need for this check
            using __has_customizations =
              __call_result_t<__has_algorithm_customizations_t, _Scheduler>;
            static_assert(
              !__has_customizations{},
              "For now the default exec::on implementation doesn't support scheduling "
              "onto schedulers that customize algorithms.");
            return __start_on_sender{(_Scheduler&&) __sched, (_Sender&&) __sndr};
          }
      };

    template <class _Env, class _SchedulerId>
      struct __with_sched_env : _Env {
        using _Scheduler = stdexec::__t<_SchedulerId>;
        _Scheduler __sched_;
        friend _Scheduler tag_invoke(get_scheduler_t, const __with_sched_env& __self) noexcept {
          return __self.__sched_;
        }
      };

    template <class _Scheduler>
      struct __with_sched_kernel : __default_kernel {
        _Scheduler __sched_;
        __with_sched_kernel(_Scheduler __sched)
          : __sched_((_Scheduler&&) __sched)
        {}

        template <class _Env>
          __with_sched_env<_Env, __id<_Scheduler>> get_env(_Env __env) {
            return {(_Env&&) __env, __sched_};
          }

        template <class _Env, class _OtherSchedulerId>
          _Env get_env(__with_sched_env<_Env, _OtherSchedulerId> __env) {
            return (_Env&&) __env;
          }
      };

    template <class _SenderId, class _Scheduler>
      struct __with_sched
        : __sender_facade<
            __with_sched<_SenderId, _Scheduler>,
            __t<_SenderId>,
            __with_sched_kernel<_Scheduler>>
      {};

    template <class _Scheduler, class _Closure>
      struct __continue_on_kernel : __default_kernel {
        _Scheduler __sched_;
        _Closure __closure_;
        __continue_on_kernel(_Scheduler __sched, _Closure __closure)
          : __sched_((_Scheduler&&) __sched)
          , __closure_((_Closure&&) __closure)
        {}

        template <class... _Ts>
          using __self_t = __make_dependent_on<__continue_on_kernel, _Ts...>;

        template <class _Sender, class _OldScheduler>
          auto transform_sender_(_Sender&& __sndr, _OldScheduler __old_sched) {
            using __sender_t = __t<__with_sched<__id<decay_t<_Sender>>, _OldScheduler>>;
            return __sender_t{(_Sender&&) __sndr, __old_sched}
              | transfer(__sched_)
              | (__member_t<_Sender, _Closure>&&) __closure_
              | transfer(__old_sched);
          }

        template <class _Sender, class _Receiver, class... _Ts>
          using __new_sender_t =
            decltype(__declval<__self_t<_Ts...>&>().transform_sender_(
              __declval<_Sender>(),
              __declval<__call_result_t<get_scheduler_t, env_of_t<_Receiver>>>()));

        template <class _Sender, class _Receiver>
            requires __valid<__new_sender_t, _Sender, _Receiver>
          auto transform_sender(_Sender&& __sndr, __ignore, _Receiver& __rcvr)
              -> __new_sender_t<_Sender, _Receiver> {
            auto __sched = get_scheduler(stdexec::get_env(__rcvr));
            return transform_sender_((_Sender&&) __sndr, __sched);
          }

        template <class _Sender, class _Receiver>
          auto transform_sender(_Sender&& __sndr, __ignore, _Receiver& __rcvr) {
            return _FAILURE_TO_CONNECT_::_WHAT_<
              _ENVIRONMENT_HAS_NO_SCHEDULER_FOR_THE_ON_ADAPTOR_TO_TRANSITION_BACK_TO<env_of_t<_Receiver>, _Sender>>{};
          }

        template <class _Env>
          __with_sched_env<_Env, __id<_Scheduler>> get_env(_Env __env) {
            return {(_Env&&) __env, __sched_};
          }
      };

    template <class _SenderId, class _Scheduler, class _Closure>
      struct __continue_on
        : __sender_facade<
            __continue_on<_SenderId, _Scheduler, _Closure>,
            __t<_SenderId>,
            __continue_on_kernel<_Scheduler, _Closure>>
      {};

    template <class _Sender, class _Scheduler, class _Closure>
      using __continue_on_t =
        __t<__continue_on<__id<decay_t<_Sender>>, _Scheduler, _Closure>>;

    template <>
      struct on_t<on_kind::continue_on> {
        template <sender _Sender, scheduler _Scheduler, __sender_adaptor_closure_for<_Sender> _Closure>
            requires constructible_from<decay_t<_Sender>, _Sender>
          auto operator()(_Sender&& __sndr, _Scheduler __sched, _Closure __closure) const {
            return __continue_on_t<_Sender, _Scheduler, _Closure>{
              (_Sender&&) __sndr,
              (_Scheduler&&) __sched,
              (_Closure&&) __closure
            };
          }

        template <scheduler _Scheduler, __sender_adaptor_closure _Closure>
          auto operator()(_Scheduler __sched, _Closure __closure) const
            -> __binder_back<on_t, _Scheduler, _Closure> {
            return {{}, {}, {(_Scheduler&&) __sched, (_Closure&&) __closure}};
          }
      };

    struct __on_t
      : on_t<on_kind::start_on>
      , on_t<on_kind::continue_on> {
      using on_t<on_kind::start_on>::operator();
      using on_t<on_kind::continue_on>::operator();
    };
  } // namespace __on

  using __on::on_kind;
  using __on::on_t;
  inline constexpr __on::__on_t on{};
} // namespace exec
