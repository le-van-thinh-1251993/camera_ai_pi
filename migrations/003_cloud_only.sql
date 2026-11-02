-- Cloud-only inference, two-second frame batches, permanent failures and shared voices.
begin;
alter table public.ai_requests add column if not exists input jsonb not null default '{}'::jsonb;

create or replace function public.ai_submit(p_id uuid,p_device text,p_image text,p_input jsonb)
returns uuid language plpgsql security definer set search_path='' as $$
declare d public.ai_devices; r public.ai_requests; frame jsonb; n integer; ms integer; previous integer := -1;
begin
 select * into d from public.ai_devices where id=p_device and user_id=auth.uid() and enabled;
 if not found then raise exception 'device_not_authorized'; end if;
 if p_image<>d.user_id::text||'/'||p_id::text||'.jpg' then raise exception 'invalid_image_path'; end if;
 if jsonb_typeof(p_input)<>'object' or octet_length(p_input::text)>4096 then raise exception 'invalid_input'; end if;
 if coalesce(p_input->>'mode','manual') not in ('auto','manual','upload') then raise exception 'invalid_mode'; end if;
 if jsonb_typeof(p_input->'frames')<>'array' then raise exception 'frames_required'; end if;
 n=jsonb_array_length(p_input->'frames');
 if n not in (1,3) or (p_input->>'mode'='auto' and n<>3) then raise exception 'invalid_frame_count'; end if;
 for frame in select value from jsonb_array_elements(p_input->'frames') loop
  if (frame->>'path')<>p_image and (frame->>'path') not in (d.user_id::text||'/'||p_id::text||'/1.jpg',d.user_id::text||'/'||p_id::text||'/2.jpg') then raise exception 'invalid_frame_path'; end if;
  ms=(frame->>'ms')::integer;
  if ms<0 or ms>6000 or ms<=previous then raise exception 'invalid_capture_time'; end if;
  previous=ms;
  if not exists(select 1 from storage.objects where bucket_id='ai-images' and name=frame->>'path') then raise exception 'image_missing'; end if;
 end loop;
 if (p_input->'frames'->0->>'path')<>p_image then raise exception 'primary_frame_required'; end if;
 if p_input->>'mode'='auto' and previous-(p_input->'frames'->0->>'ms')::integer<2000 then raise exception 'hold_too_short'; end if;
 insert into public.ai_requests(id,device_id,owner_id,image_path,input)
 values(p_id,p_device,d.user_id,p_image,p_input) on conflict(id) do nothing;
 select * into r from public.ai_requests where id=p_id;
 if r.owner_id<>d.user_id or r.device_id<>p_device or r.image_path<>p_image or r.input<>p_input then raise exception 'request_id_conflict'; end if;
 update public.ai_devices set last_seen=now() where id=p_device;
 return p_id;
end $$;
revoke all on function public.ai_submit(uuid,text,text,jsonb) from public,anon;
grant execute on function public.ai_submit(uuid,text,text,jsonb) to authenticated;

create or replace function public.ai_complete(p_id uuid,p_token uuid,p_result jsonb,p_audio text,p_screen text)
returns boolean language plpgsql security definer set search_path='' as $$
declare own uuid;
begin
 select owner_id into own from public.ai_requests where id=p_id;
 if p_audio is not null and p_audio not like own::text||'/'||p_id::text||'/%'
 and p_audio !~ ('^'||own::text||'/voices/[a-f0-9]{64}\.wav$') then raise exception 'invalid_audio_path'; end if;
 if p_screen is not null and p_screen not like own::text||'/'||p_id::text||'/%' then raise exception 'invalid_screen_path'; end if;
 update public.ai_requests set status='completed',result=p_result,audio_path=p_audio,screen_path=p_screen,
 completed_at=now(),lease_until=null,lease_token=null,error_code=null
 where id=p_id and status='processing' and lease_token=p_token and lease_until>now();
 return found;
end $$;

create or replace function public.ai_fail_v2(p_id uuid,p_token uuid,p_error text,p_permanent boolean)
returns boolean language plpgsql security definer set search_path='' as $$
begin
 update public.ai_requests set status=case when p_permanent or attempts>=5 then 'failed' else 'pending' end,
 error_code=left(p_error,100),lease_until=null,lease_token=null
 where id=p_id and status='processing' and lease_token=p_token and lease_until>now();
 return found;
end $$;
revoke all on function public.ai_fail_v2(uuid,uuid,text,boolean) from public,anon,authenticated;
grant execute on function public.ai_fail_v2(uuid,uuid,text,boolean) to service_role;

create or replace function public.ai_device_exchange(p_device text,p_state jsonb)
returns jsonb language plpgsql security definer set search_path='' as $$
declare command jsonb;
begin
 if jsonb_typeof(p_state)<>'object' or octet_length(p_state::text)>4096 then raise exception 'invalid_state'; end if;
 update public.ai_devices set state=p_state,last_seen=now()
 where id=p_device and user_id=auth.uid() and enabled returning control into command;
 if not found then raise exception 'device_not_authorized'; end if;
 if coalesce((command->>'expires')::numeric,0)<extract(epoch from now()) then command='{}'::jsonb; end if;
 return jsonb_build_object('control',command,'server_time',extract(epoch from now()));
end $$;
revoke all on function public.ai_device_exchange(text,jsonb) from public,anon;
grant execute on function public.ai_device_exchange(text,jsonb) to authenticated;
commit;
