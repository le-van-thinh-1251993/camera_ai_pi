-- Add dashboard control and device status without changing request processing.
begin;
alter table public.ai_devices add column if not exists control jsonb not null default '{}'::jsonb;
alter table public.ai_devices add column if not exists state jsonb not null default '{}'::jsonb;
create or replace function public.ai_device_report(p_device text,p_state jsonb)
returns boolean language plpgsql security definer set search_path='' as $$
begin
 if jsonb_typeof(p_state)<>'object' or octet_length(p_state::text)>4096 then raise exception 'invalid_device_state'; end if;
 update public.ai_devices set state=p_state,last_seen=now()
 where id=p_device and user_id=auth.uid() and enabled;
 return found;
end $$;
revoke all on function public.ai_device_report(text,jsonb) from public,anon;
grant execute on function public.ai_device_report(text,jsonb) to authenticated;
commit;
