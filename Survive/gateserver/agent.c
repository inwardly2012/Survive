#include "agent.h"
#include "common/cmdhandler.h"
#include "common/netcmd.h"
#include "gateplayer.h"
#include "chanmsg.h"
#include "togrpgame.h"
#include "kn_thread.h"
#include "config.h"
#include "gateserver.h"

#define MAXCMD 65535

static cmd_handler_t handler[MAXCMD] = {NULL};

static __thread agent* t_agent = NULL;


void release_agent_player(agentplayer_t player){
	t_agent->players[player->agentsession.sessionid] = NULL;
	release_id(t_agent->idmgr,player->agentsession.sessionid);
	if(player->actname){
		kn_release_string(player->actname);
	}
	free(player);
}

agentplayer_t new_agent_player(kn_stream_conn_t conn){
	int id = get_id(t_agent->idmgr);
	if(id <= 0) return NULL;
	else{
		agentplayer_t player = calloc(1,sizeof(*player));
		player->toclient = conn;
		kn_ref_init(player->ref,(void (*)(void*))release_agent_player);
		player->agentsession.data = player->ref.identity;
		player->agentsession.aid = t_agent->idx;
		player->agentsession.sessionid = id;
		t_agent->players[id] = player;
		return player;
	}
}

static void forward_game(kn_stream_conn_t con,rpacket_t rpk){
	agentplayer_t ply = (agentplayer_t)kn_stream_conn_getud(con);
	wpacket_t wpk = wpk_create_by_rpacket(rpk);
	wpk_write_uint32(wpk,ply->gameid);
	struct chanmsg_forward_game *msg = calloc(1,sizeof(*msg));
	msg->chanmsg.msgtype = FORWARD_GAME;
	msg->game = ply->togame;
	msg->wpk = wpk;
	kn_channel_putmsg(g_togrpgame->chan,NULL,msg,chanmsg_forward_game_destroy);
}

static void forward_group(kn_stream_conn_t con,rpacket_t rpk){
	agentplayer_t ply = (agentplayer_t)kn_stream_conn_getud(con);
	wpacket_t wpk = wpk_create_by_rpacket(rpk);
	wpk_write_uint32(wpk,ply->groupid);
	struct chanmsg_forward_group *msg = calloc(1,sizeof(*msg));
	msg->chanmsg.msgtype = FORWARD_GROUP;
	msg->wpk = wpk;
	kn_channel_putmsg(g_togrpgame->chan,NULL,msg,chanmsg_forward_group_destroy);
}


//处理来自客户端的网络包
static int on_packet(kn_stream_conn_t con,rpacket_t rpk){
	uint16_t cmd = rpk_peek_uint16(rpk);
	if(cmd > CMD_CA_BEGIN && cmd < CMD_CA_END){
		rpk_read_uint16(rpk);
		if(handler[cmd]->_fn) handler[cmd]->_fn(rpk,con);
	}else if(cmd > CMD_CS_BEGIN && cmd < CMD_CS_END){
		 //转发到gameserver
		forward_game(con,rpk);
	}else if(cmd > CMD_CG_BEGIN && cmd < CMD_CG_END){
		//转发到groupserver
		forward_group(con,rpk);
	}
	return 1;
}

static void on_disconnected(kn_stream_conn_t conn,int err){
	agentplayer_t player = kn_stream_conn_getud(conn);
	if(player){
		if(player->groupid){
			//通知groupserver player的连接断开
		
		}
		if(player->gameid){
			//通知gameserver player的连接断开
		
		}
		kn_ref_release((kn_ref*)player);
	}
}

//处理来自channel的消息
static void on_channel_msg(kn_channel_t chan, kn_channel_t from,void *msg,void *_)
{
	(void)_;
	if(((struct chanmsg*)msg)->msgtype == NEWCLIENT){
		struct chanmsg_newclient *_msg = (struct chanmsg_newclient*)msg;
		agentplayer_t player = new_agent_player(_msg->conn);
		if(player){
			if(0 == kn_stream_server_bind(t_agent->server,_msg->conn,1,4096,on_packet,on_disconnected,
								  10*1000,NULL,0,NULL)){
				kn_stream_conn_setud(_msg->conn,player);
				_msg->conn = NULL;
			}else{
				kn_ref_release((kn_ref*)player);
			}
		}
	}
}


int    connect_redis();

static void on_redis_connect(redisconn_t conn,int err,void *_){
	(void)_;
	if(conn)
		t_agent->redis = conn;
	else{
		connect_redis();	
	}
}

static	void on_redis_disconnected(redisconn_t conn,void *_){
	(void)_;
	t_agent->redis = NULL;
	connect_redis();	
}

static void *service_main(void *ud){
	printf("agent service运行\n");	
	t_agent = (agent*)ud;
	if(0 != connect_redis()){
		LOG_GATE(LOG_ERROR,"connect to redis failed,agent thread exit,agentid[%u]\n",t_agent->idx);	
		return NULL;
	}
	while(!t_agent->stop){
		kn_proactor_run(t_agent->p,50);
	}
	return NULL;
}

int    connect_redis(){
	if(0 != kn_redisAsynConnect(t_agent->p,
				kn_to_cstr(g_config->redisip),g_config->redisport,
				on_redis_connect,
				on_redis_disconnected,
				NULL)){
		//记录日志
		LOG_GATE(LOG_ERROR,"kn_redisAsynConnect return not 0\n");				
		return -1;
	}
	return 0;
}


agent *start_agent(uint8_t idx){
	agent *agent = calloc(1,sizeof(*agent));
	agent->idx = idx;
	agent->p = kn_new_proactor();
	agent->t = kn_create_thread(THREAD_JOINABLE);
	agent->idmgr = new_idmgr(1,4095);
	kn_new_stream_server(agent->p,NULL,NULL);
	agent->chan = kn_new_channel(kn_thread_getid(agent->t));
	kn_channel_bind(agent->p,agent->chan,on_channel_msg,NULL);
	kn_thread_start_run(agent->t,service_main,agent);
	return agent;
}

void   stop_agent(agent *agent){
	agent->stop = 1;
	kn_thread_join(agent->t);
	//stop_agent应该在进程结束时调用，不做任何收尾工作了
}
