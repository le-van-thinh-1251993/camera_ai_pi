-- Prevent the still-running v5 Pi from consuming three-frame v6 jobs.
begin;
create or replace function public.ai_claim(p_worker text,p_seconds integer default 120)
returns setof public.ai_requests language plpgsql security definer set search_path='' as $$
declare job uuid;
begin
 update public.ai_requests set status='failed',error_code='retry_limit',lease_token=null,lease_until=null
 where status='processing' and lease_until<now() and attempts>=5;
 select id into job from public.ai_requests
 where (status='pending' or(status='processing' and lease_until<now())) and attempts<5
 and (input->>'pipeline_version') is distinct from '6'
 order by created_at for update skip locked limit 1;
 if job is null then return; end if;
 return query update public.ai_requests set status='processing',worker_id=left(p_worker,100),
 lease_token=gen_random_uuid(),lease_until=now()+make_interval(secs=>greatest(30,least(p_seconds,600))),attempts=attempts+1
 where id=job returning *;
end $$;
create or replace function public.ai_claim_v6(p_worker text,p_seconds integer default 120)
returns setof public.ai_requests language plpgsql security definer set search_path='' as $$
declare job uuid;
begin
 update public.ai_requests set status='failed',error_code='retry_limit',lease_token=null,lease_until=null
 where status='processing' and lease_until<now() and attempts>=5 and input->>'pipeline_version'='6';
 select id into job from public.ai_requests
 where (status='pending' or(status='processing' and lease_until<now())) and attempts<5
 and input->>'pipeline_version'='6'
 order by created_at for update skip locked limit 1;
 if job is null then return; end if;
 return query update public.ai_requests set status='processing',worker_id=left(p_worker,100),
 lease_token=gen_random_uuid(),lease_until=now()+make_interval(secs=>greatest(30,least(p_seconds,600))),attempts=attempts+1
 where id=job returning *;
end $$;
revoke all on function public.ai_claim_v6(text,integer) from public,anon,authenticated;
grant execute on function public.ai_claim_v6(text,integer) to service_role;
commit;
