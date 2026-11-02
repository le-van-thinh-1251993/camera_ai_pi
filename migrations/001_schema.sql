-- AI Camera Hybrid Cloud v1. Run once in Supabase SQL Editor.
begin;
create table if not exists public.ai_devices (
 id text primary key check (id ~ '^[A-Za-z0-9_-]{1,32}$'),
 user_id uuid unique not null references auth.users(id),
 enabled boolean not null default true,
 last_seen timestamptz,
 created_at timestamptz not null default now()
);
create table if not exists public.ai_requests (
 id uuid primary key,
 device_id text not null references public.ai_devices(id),
 owner_id uuid not null references auth.users(id),
 image_path text not null,
 status text not null default 'pending' check(status in ('pending','processing','completed','failed','acked')),
 result jsonb,
 audio_path text,
 screen_path text,
 attempts integer not null default 0,
 worker_id text,
 lease_token uuid,
 lease_until timestamptz,
 error_code text,
 created_at timestamptz not null default now(),
 completed_at timestamptz,
 acked_at timestamptz,
 ack_played boolean,
 ack_displayed boolean
);
create index if not exists ai_requests_queue on public.ai_requests(status,created_at);
alter table public.ai_devices enable row level security;
alter table public.ai_requests enable row level security;
revoke all on public.ai_devices,public.ai_requests from anon,authenticated;
grant select on public.ai_devices,public.ai_requests to authenticated;
grant all on public.ai_devices,public.ai_requests to service_role;
drop policy if exists device_read_self on public.ai_devices;
create policy device_read_self on public.ai_devices for select to authenticated using(user_id=(select auth.uid()));
drop policy if exists request_read_self on public.ai_requests;
create policy request_read_self on public.ai_requests for select to authenticated using(owner_id=(select auth.uid()));

create or replace function public.ai_enqueue(p_id uuid,p_device text,p_image text)
returns uuid language plpgsql security definer set search_path='' as $$
declare d public.ai_devices; r public.ai_requests;
begin
 select * into d from public.ai_devices where id=p_device and user_id=auth.uid() and enabled;
 if not found then raise exception 'device_not_authorized'; end if;
 if p_image<>d.user_id::text||'/'||p_id::text||'.jpg' then raise exception 'invalid_image_path'; end if;
 if not exists(select 1 from storage.objects where bucket_id='ai-images' and name=p_image) then raise exception 'image_missing'; end if;
 insert into public.ai_requests(id,device_id,owner_id,image_path) values(p_id,p_device,d.user_id,p_image) on conflict(id) do nothing;
 select * into r from public.ai_requests where id=p_id;
 if r.owner_id<>d.user_id or r.device_id<>p_device or r.image_path<>p_image then raise exception 'request_id_conflict'; end if;
 update public.ai_devices set last_seen=now() where id=p_device;
 return p_id;
end $$;

create or replace function public.ai_claim(p_worker text,p_seconds integer default 120)
returns setof public.ai_requests language plpgsql security definer set search_path='' as $$
declare job uuid;
begin
 update public.ai_requests set status='failed',error_code='retry_limit',lease_token=null,lease_until=null
 where status='processing' and lease_until<now() and attempts>=5;
 select id into job from public.ai_requests
 where (status='pending' or(status='processing' and lease_until<now())) and attempts<5
 order by created_at for update skip locked limit 1;
 if job is null then return; end if;
 return query update public.ai_requests set status='processing',worker_id=left(p_worker,100),
 lease_token=gen_random_uuid(),lease_until=now()+make_interval(secs=>greatest(30,least(p_seconds,600))),attempts=attempts+1
 where id=job returning *;
end $$;

create or replace function public.ai_renew(p_id uuid,p_token uuid)
returns boolean language plpgsql security definer set search_path='' as $$
begin
 update public.ai_requests set lease_until=now()+interval '120 seconds'
 where id=p_id and status='processing' and lease_token=p_token and lease_until>now();
 return found;
end $$;

create or replace function public.ai_complete(p_id uuid,p_token uuid,p_result jsonb,p_audio text,p_screen text)
returns boolean language plpgsql security definer set search_path='' as $$
declare own uuid;
begin
 select owner_id into own from public.ai_requests where id=p_id;
 if p_audio is not null and p_audio not like own::text||'/'||p_id::text||'/%' then raise exception 'invalid_audio_path'; end if;
 if p_screen is not null and p_screen not like own::text||'/'||p_id::text||'/%' then raise exception 'invalid_screen_path'; end if;
 update public.ai_requests set status='completed',result=p_result,audio_path=p_audio,screen_path=p_screen,
 completed_at=now(),lease_until=null,lease_token=null,error_code=null
 where id=p_id and status='processing' and lease_token=p_token and lease_until>now();
 return found;
end $$;

create or replace function public.ai_fail(p_id uuid,p_token uuid,p_error text)
returns boolean language plpgsql security definer set search_path='' as $$
begin
 update public.ai_requests set status=case when attempts>=5 then 'failed' else 'pending' end,
 error_code=left(p_error,100),lease_until=null,lease_token=null
 where id=p_id and status='processing' and lease_token=p_token and lease_until>now();
 return found;
end $$;

create or replace function public.ai_ack(p_id uuid,p_displayed boolean,p_played boolean)
returns boolean language plpgsql security definer set search_path='' as $$
begin
 update public.ai_requests set status='acked',acked_at=coalesce(acked_at,now()),
 ack_displayed=p_displayed,ack_played=p_played
 where id=p_id and owner_id=auth.uid() and status in ('completed','acked');
 return found;
end $$;

revoke all on function public.ai_enqueue(uuid,text,text) from public,anon;
revoke all on function public.ai_ack(uuid,boolean,boolean) from public,anon;
grant execute on function public.ai_enqueue(uuid,text,text),public.ai_ack(uuid,boolean,boolean) to authenticated;
revoke all on function public.ai_claim(text,integer),public.ai_renew(uuid,uuid),public.ai_complete(uuid,uuid,jsonb,text,text),public.ai_fail(uuid,uuid,text) from public,anon,authenticated;
grant execute on function public.ai_claim(text,integer),public.ai_renew(uuid,uuid),public.ai_complete(uuid,uuid,jsonb,text,text),public.ai_fail(uuid,uuid,text) to service_role;

insert into storage.buckets(id,name,public,file_size_limit,allowed_mime_types)
values ('ai-images','ai-images',false,15728640,array['image/jpeg','image/png']),
('ai-results','ai-results',false,2097152,array['audio/wav','audio/mpeg','image/jpeg'])
on conflict(id) do nothing;
drop policy if exists ai_image_upload on storage.objects;
create policy ai_image_upload on storage.objects for insert to authenticated
with check(bucket_id='ai-images' and (storage.foldername(name))[1]=(select auth.uid())::text
 and exists(select 1 from public.ai_devices where user_id=(select auth.uid()) and enabled));
drop policy if exists ai_storage_read on storage.objects;
create policy ai_storage_read on storage.objects for select to authenticated
using(bucket_id in ('ai-images','ai-results') and (storage.foldername(name))[1]=(select auth.uid())::text);
commit;
