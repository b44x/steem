
#include <steem/plugins/block_data_export/block_data_export_plugin.hpp>

#include <steem/plugins/rc/rc_export_objects.hpp>
#include <steem/plugins/rc/rc_plugin.hpp>
#include <steem/plugins/rc/rc_objects.hpp>

#include <steem/chain/account_object.hpp>
#include <steem/chain/database.hpp>
#include <steem/chain/database_exceptions.hpp>
#include <steem/chain/index.hpp>
#include <steem/chain/operation_notification.hpp>

#include <steem/jsonball/jsonball.hpp>

#define STEEM_RC_REGEN_TIME   (60*60*24*5)

namespace steem { namespace plugins { namespace rc {

using steem::plugins::block_data_export::block_data_export_plugin;

namespace detail {

using chain::plugin_exception;
using steem::chain::util::manabar_params;

struct visitor_shared_state
{
   std::vector< account_name_type >   regen_accounts;

   void clear()
   {  regen_accounts.clear();   }
};

class rc_plugin_impl
{
   public:
      rc_plugin_impl( rc_plugin& _plugin ) :
         _db( appbase::app().get_plugin< steem::plugins::chain::chain_plugin >().db() ),
         _self( _plugin ) {}

      void on_post_apply_block( const block_notification& note );
      void on_pre_apply_transaction( const transaction_notification& note );
      void on_post_apply_transaction( const transaction_notification& note );
      void on_pre_apply_operation( const operation_notification& note );
      void on_post_apply_operation( const operation_notification& note );
      void on_first_block();

      database&                     _db;
      rc_plugin&                    _self;

      // State is shared between the pre- and post- visitor
      visitor_shared_state          _shared_state;

      boost::signals2::connection   _post_apply_block_conn;
      boost::signals2::connection   _pre_apply_transaction_conn;
      boost::signals2::connection   _post_apply_transaction_conn;
      boost::signals2::connection   _pre_apply_operation_conn;
      boost::signals2::connection   _post_apply_operation_conn;
};

inline int64_t get_next_vesting_withdrawal( const account_object& account )
{
   int64_t total_left = account.to_withdraw.value - account.withdrawn.value;
   int64_t withdraw_per_period = account.vesting_withdraw_rate.amount.value;
   int64_t next_withdrawal = (withdraw_per_period <= total_left) ? withdraw_per_period : total_left;
   bool is_done = (account.next_vesting_withdrawal == fc::time_point_sec::maximum());
   return is_done ? 0 : next_withdrawal;
}

int64_t get_maximum_rc( const account_object& account, const rc_account_object& rc_account )
{
   int64_t result = account.vesting_shares.amount.value;
   result = fc::signed_sat_sub( result, account.delegated_vesting_shares.amount.value );
   result = fc::signed_sat_add( result, account.received_vesting_shares.amount.value );
   result = fc::signed_sat_add( result, rc_account.max_rc_creation_adjustment.amount.value );
   result = fc::signed_sat_sub( result, get_next_vesting_withdrawal( account ) );
   return result;
}

template< bool account_may_exist = false >
void create_rc_account( database& db, uint32_t now, const account_object& account, asset max_rc_creation_adjustment )
{
   if( account_may_exist )
   {
      const rc_account_object* rc_account = db.find< rc_account_object, by_name >( account.name );
      if( rc_account != nullptr )
         return;
   }

   db.create< rc_account_object >( [&]( rc_account_object& rca )
   {
      rca.account = account.name;
      rca.rc_manabar.current_mana = get_maximum_rc( account, rca );
      rca.rc_manabar.last_update_time = now;
      rca.max_rc_creation_adjustment = max_rc_creation_adjustment;
      rca.max_rc = rca.rc_manabar.current_mana;
   } );
}

template< bool account_may_exist = false >
void create_rc_account( database& db, uint32_t now, const account_name_type& account_name, asset max_rc_creation_adjustment )
{
   create_rc_account< account_may_exist >( db, now, account_name, max_rc_creation_adjustment );
}

std::vector< std::pair< int64_t, account_name_type > > dump_all_accounts( const database& db )
{
   std::vector< std::pair< int64_t, account_name_type > > result;
   const auto& idx = db.get_index< account_index >().indices().get< by_id >();
   for( auto it=idx.begin(); it!=idx.end(); ++it )
   {
      result.emplace_back( it->id._id, it->name );
   }

   return result;
}

std::vector< std::pair< int64_t, account_name_type > > dump_all_rc_accounts( const database& db )
{
   std::vector< std::pair< int64_t, account_name_type > > result;
   const auto& idx = db.get_index< rc_account_index >().indices().get< by_id >();
   for( auto it=idx.begin(); it!=idx.end(); ++it )
   {
      result.emplace_back( it->id._id, it->account );
   }

   return result;
}

account_name_type get_resource_user( const signed_transaction& tx )
{
   flat_set< account_name_type > req_active;
   flat_set< account_name_type > req_owner;
   flat_set< account_name_type > req_posting;
   vector< authority > other;

   for( const operation& op : tx.operations )
   {
      operation_get_required_authorities( op, req_active, req_owner, req_posting, other );
      for( const account_name_type& account : req_active )
         return account;
      for( const account_name_type& account : req_owner )
         return account;
      for( const account_name_type& account : req_posting )
         return account;
   }
   return account_name_type();
}

void use_account_rcs(
   database& db,
   const dynamic_global_property_object& gpo,
   const account_name_type& account_name,
   int64_t rc )
{
   if( account_name == account_name_type() )
   {
      if( db.is_producing() )
      {
         STEEM_ASSERT( false, plugin_exception,
            "Tried to execute transaction with no resource user",
            );
      }
      return;
   }

   // ilog( "use_account_rcs( ${n}, ${rc} )", ("n", account_name)("rc", rc) );
   const account_object& account = db.get< account_object, by_name >( account_name );
   const rc_account_object& rc_account = db.get< rc_account_object, by_name >( account_name );

   manabar_params mbparams;
   mbparams.max_mana = get_maximum_rc( account, rc_account );
   mbparams.regen_time = STEEM_RC_REGEN_TIME;

   db.modify( rc_account, [&]( rc_account_object& rca )
   {
      rca.rc_manabar.regenerate_mana( mbparams, gpo.time.sec_since_epoch() );

      if( db.is_producing() )
      {
         STEEM_ASSERT( rc_account.rc_manabar.has_mana( rc ), plugin_exception,
            "Account: ${account} needs ${rc_needed} RC. Please wait to transact, or power up STEEM.",
            ("account", account_name)
            ("rc_needed", rc)
            );
      }

      rca.rc_manabar.use_mana( rc );
   } );
}

void rc_plugin_impl::on_post_apply_transaction( const transaction_notification& note )
{
   const dynamic_global_property_object& gpo = _db.get_dynamic_global_properties();
   bool debug_print = (gpo.head_block_number > 160785) && (gpo.head_block_number < 160795);
   if( debug_print )
   {
      ilog( "processing tx: ${txid} ${tx}", ("txid", note.transaction_id)("tx", note.transaction) );
   }
   int64_t rc_regen = gpo.total_vesting_shares.amount.value / STEEM_RC_REGEN_TIME;

   rc_transaction_info tx_info;

   // How many resources does the transaction use?
   count_resources( note.transaction, tx_info.usage );

   // How many RC does this transaction cost?
   const rc_resource_param_object& params_obj = _db.get< rc_resource_param_object, by_id >( rc_resource_param_object::id_type() );
   const rc_pool_object& pool_obj = _db.get< rc_pool_object, by_id >( rc_pool_object::id_type() );

   int64_t total_cost = 0;

   // When rc_regen is 0, everything is free
   if( rc_regen > 0 )
   {
      for( size_t i=0; i<STEEM_NUM_RESOURCE_TYPES; i++ )
      {
         const rc_resource_params& params = params_obj.resource_param_array[i];
         int64_t pool = pool_obj.pool_array[i];

         tx_info.usage.resource_count[i] *= int64_t( params.resource_unit );
         tx_info.cost[i] = compute_rc_cost_of_resource( params.curve_params, pool, tx_info.usage.resource_count[i], rc_regen );
         total_cost += tx_info.cost[i];
      }
   }

   tx_info.resource_user = get_resource_user( note.transaction );
   use_account_rcs( _db, gpo, tx_info.resource_user, total_cost );

   std::shared_ptr< exp_rc_data > export_data =
      steem::plugins::block_data_export::find_export_data< exp_rc_data >( STEEM_RC_PLUGIN_NAME );
   if( (gpo.head_block_number % 10000) == 0 )
   {
      ilog( "${t} : ${i}", ("t", gpo.time)("i", tx_info) );
   }
   if( export_data )
      export_data->tx_info.push_back( tx_info );
}

void rc_plugin_impl::on_post_apply_block( const block_notification& note )
{
   const dynamic_global_property_object& gpo = _db.get_dynamic_global_properties();

   if( gpo.head_block_number == 1 )
   {
      on_first_block();
   }

   if( gpo.total_vesting_shares.amount <= 0 )
   {
      return;
   }

   // How many resources did transactions use?
   count_resources_result count;
   for( const signed_transaction& tx : note.block.transactions )
   {
      count_resources( tx, count );
   }

   const rc_resource_param_object& params_obj = _db.get< rc_resource_param_object, by_id >( rc_resource_param_object::id_type() );

   rc_block_info block_info;

   _db.modify( _db.get< rc_pool_object, by_id >( rc_pool_object::id_type() ),
      [&]( rc_pool_object& pool_obj )
      {
         bool debug_print = ((gpo.head_block_number % 10000) == 0);

         for( size_t i=0; i<STEEM_NUM_RESOURCE_TYPES; i++ )
         {
            const rc_resource_params& params = params_obj.resource_param_array[i];
            int64_t& pool = pool_obj.pool_array[i];
            uint32_t dt = 0;

            block_info.pool[i] = pool;
            switch( params.time_unit )
            {
               case rc_time_unit_blocks:
                  dt = 1;
                  break;
               case rc_time_unit_seconds:
                  dt = gpo.time.sec_since_epoch() - pool_obj.last_update.sec_since_epoch();
                  break;
               default:
                  FC_ASSERT( false, "unknown time unit in RC parameter object" );
            }
            block_info.dt[i] = dt;

            block_info.decay[i] = compute_pool_decay( params.decay_params, pool, dt );
            block_info.budget[i] = int64_t( params.budget_per_time_unit ) * int64_t( dt );
            block_info.usage[i] = count.resource_count[i]*int64_t( params.resource_unit );

            pool = pool - block_info.decay[i] + block_info.budget[i] - block_info.usage[i];

            if( debug_print )
            {
               double k = 27.027027027027028;
               double a = double(params.pool_eq - pool);
               a /= k*double(pool);
               ilog( "a=${a}   aR=${aR}", ("a", a)("aR", a*gpo.total_vesting_shares.amount.value/STEEM_RC_REGEN_TIME) );
            }
         }
         if( debug_print )
         {
            ilog( "${t} : ${i}", ("t", gpo.time)("i", block_info) );
         }
         pool_obj.last_update = gpo.time;
      } );

   std::shared_ptr< exp_rc_data > export_data =
      steem::plugins::block_data_export::find_export_data< exp_rc_data >( STEEM_RC_PLUGIN_NAME );
   if( export_data )
      export_data->block_info = block_info;
}

void rc_plugin_impl::on_first_block()
{
   std::string resource_params_json = steem::jsonball::get_resource_parameters();
   fc::variant resource_params_var = fc::json::from_string( resource_params_json, fc::json::strict_parser );
   std::vector< std::pair< fc::variant, std::pair< fc::variant_object, fc::variant_object > > > resource_params_pairs;
   fc::from_variant( resource_params_var, resource_params_pairs );
   fc::time_point_sec now = _db.get_dynamic_global_properties().time;

   _db.create< rc_resource_param_object >(
      [&]( rc_resource_param_object& params_obj )
      {
         for( auto& kv : resource_params_pairs )
         {
            auto k = kv.first.as< rc_resource_types >();
            fc::variant_object& vo = kv.second.first;
            fc::mutable_variant_object mvo(vo);
            mvo["time_unit"] = int8_t( vo["time_unit"].as< rc_time_unit_type >() );
            fc::from_variant( fc::variant( mvo ), params_obj.resource_param_array[ k ] );
         }

         ilog( "Genesis params_obj is ${o}", ("o", params_obj) );
      } );

   const rc_resource_param_object& params_obj = _db.get< rc_resource_param_object, by_id >( rc_resource_param_object::id_type() );

   _db.create< rc_pool_object >(
      [&]( rc_pool_object& pool_obj )
      {
         for( size_t i=0; i<STEEM_NUM_RESOURCE_TYPES; i++ )
         {
            const rc_resource_params& params = params_obj.resource_param_array[i];
            pool_obj.pool_array[i] = params.pool_eq;
         }
         pool_obj.last_update = now;

         ilog( "Genesis pool_obj is ${o}", ("o", pool_obj) );
      } );

   const auto& idx = _db.get_index< account_index >().indices().get< by_id >();
   for( auto it=idx.begin(); it!=idx.end(); ++it )
   {
      create_rc_account( _db, now.sec_since_epoch(), *it, asset(0, VESTS_SYMBOL ) );
   }

   return;
}

struct get_worker_name_visitor
{
   typedef account_name_type result_type;

   template< typename WorkType >
   account_name_type operator()( const WorkType& work )
   {   return work.input.worker_account;    }
};

account_name_type get_worker_name( const pow2_work& work )
{
   // Even though in both cases the result is work.input.worker_account,
   // we have to use a visitor because pow2_work is a static_variant
   get_worker_name_visitor vtor;
   return work.visit( vtor );
}

//
// This visitor performs the following functions:
//
// - Call regenerate() when an account's vesting shares are about to change
// - Save regenerated account names in a local array for further (post-operation) processing
//
struct pre_apply_operation_visitor
{
   typedef void result_type;

   visitor_shared_state&                    _shared_state;
   database&                                _db;
   uint32_t                                 _current_time = 0;
   fc::optional< price >                    _vesting_share_price;

   pre_apply_operation_visitor(
      visitor_shared_state& ss,
      database& db,
      uint32_t t,
      const fc::optional< price >& vsp
      ) : _shared_state(ss), _db(db), _current_time(t), _vesting_share_price(vsp)
   {}

   void regenerate( const account_name_type& name )const
   {
      //
      // Since RC tracking is non-consensus, we must rely on consensus to forbid
      // transferring / delegating VESTS that haven't regenerated voting power.
      //
      // TODO:  Issue number
      //
      static_assert( STEEM_RC_REGEN_TIME <= STEEM_VOTE_REGENERATION_SECONDS, "RC regen time must be smaller than vote regen time" );

      const account_object& account = _db.get< account_object, by_name >( name );
      const rc_account_object& rc_account = _db.get< rc_account_object, by_name >( name );

      if( account.vesting_shares != rc_account.last_vesting_shares )
      {
         elog( "Account ${a} VESTS changed from ${old} to ${new} without triggering an op, noticed on block ${b}",
            ("a", name)("old", account.vesting_shares)("new", rc_account.last_vesting_shares)("b", _db.head_block_num()) );
         // TODO:  Should there be an exception thrown on this condition?
      }

      manabar_params mbparams;
      mbparams.max_mana = get_maximum_rc( account, rc_account );
      mbparams.regen_time = STEEM_RC_REGEN_TIME;

      _db.modify( rc_account, [&]( rc_account_object& rca )
      {
         rca.rc_manabar.regenerate_mana( mbparams, _current_time );
      } );

      _shared_state.regen_accounts.push_back( name );
   }

   void operator()( const transfer_to_vesting_operation& op )const
   {
      account_name_type target = op.to.size() ? op.to : op.from;
      regenerate( target );
   }

   void operator()( const withdraw_vesting_operation& op )const
   {
      regenerate( op.account );
   }

   void operator()( const set_withdraw_vesting_route_operation& op )const
   {
      regenerate( op.from_account );
   }

   void operator()( const delegate_vesting_shares_operation& op )const
   {
      regenerate( op.delegator );
      regenerate( op.delegatee );
   }

   void operator()( const author_reward_operation& op )const
   {
      regenerate( op.author );
   }

   void operator()( const curation_reward_operation& op )const
   {
      regenerate( op.curator );
   }

   // Is this one actually necessary?
   void operator()( const comment_reward_operation& op )const
   {
      regenerate( op.author );
   }

   void operator()( const fill_vesting_withdraw_operation& op )const
   {
      regenerate( op.from_account );
   }

   void operator()( const hardfork_operation& op )const
   {
      if( op.hardfork_id == STEEM_HARDFORK_0_1 )
      {
         const auto& idx = _db.get_index< account_index >().indices().get< by_id >();
         for( auto it=idx.begin(); it!=idx.end(); ++it )
         {
            regenerate( it->name );
         }
      }
   }

   void operator()( const return_vesting_delegation_operation& op )const
   {
      regenerate( op.account );
   }

   void operator()( const comment_benefactor_reward_operation& op )const
   {
      regenerate( op.benefactor );
   }

   void operator()( const producer_reward_operation& op )const
   {
      regenerate( op.producer );
   }

   template< typename Op >
   void operator()( const Op& op ) {}
};

struct post_apply_operation_visitor
{
   typedef void result_type;

   visitor_shared_state&                    _shared_state;
   database&                                _db;
   uint32_t                                 _current_time = 0;

   post_apply_operation_visitor(
      visitor_shared_state& ss,
      database& db,
      uint32_t t
      ) : _shared_state(ss), _db(db), _current_time(t)
   {}

   void operator()( const account_create_operation& op )const
   {
      create_rc_account( _db, _current_time, op.new_account_name, op.fee );
   }

   void operator()( const account_create_with_delegation_operation& op )const
   {
      create_rc_account( _db, _current_time, op.new_account_name, op.fee );
   }

   void operator()( const pow_operation& op )const
   {
      create_rc_account< true >( _db, _current_time, op.worker_account, asset( 0, STEEM_SYMBOL ) );
   }

   void operator()( const pow2_operation& op )const
   {
      create_rc_account< true >( _db, _current_time, get_worker_name( op ), asset( 0, STEEM_SYMBOL ) );
   }

   // TODO create_claimed_account_operation
};

void rc_plugin_impl::on_pre_apply_operation( const operation_notification& note )
{
   const global_property_object& gpo = _db.get_dynamic_global_properties();
   const uint32_t now = gpo.time.sec_since_epoch();
   pre_apply_operation_visitor vtor( _shared_state, _db, now, vsp );

   // Update any accounts that were created by this operation based on fee
   fc::optional< price > vsp;

   // TODO: Add issue number to HF constant
   if( _db.has_hardfork( STEEM_HARDFORK_0_20 ) )
      vsp = gpo.get_vesting_share_price();

   // If another operation somehow executed its pre-apply without ever getting to
   // post-apply (for example due to an exception), _shared_state might have unclean
   // data.  So it's cleaned here.
   _shared_state.clear();
   note.op.visit( vtor );
}

void rc_plugin_impl::on_post_apply_operation( const operation_notification& note )
{
   const global_property_object& gpo = _db.get_dynamic_global_properties();
   const uint32_t now = gpo.time.sec_since_epoch();
   post_apply_operation_visitor vtor( _shared_state, _db, now );
   note.op.visit( vtor );

   // This isn't really necessary as _shared_state will be cleared by the next
   // pre-op.
   _shared_state.clear();
}

} // detail

rc_plugin::rc_plugin() {}
rc_plugin::~rc_plugin() {}

void rc_plugin::set_program_options( options_description& cli, options_description& cfg ){}

void rc_plugin::plugin_initialize( const boost::program_options::variables_map& options )
{
   ilog( "Initializing resource credit plugin" );

   my = std::make_unique< detail::rc_plugin_impl >( *this );

   try
   {
      block_data_export_plugin* export_plugin = appbase::app().find_plugin< block_data_export_plugin >();
      if( export_plugin != nullptr )
      {
         ilog( "Registering RC export data factory" );
         export_plugin->register_export_data_factory( STEEM_RC_PLUGIN_NAME,
            []() -> std::shared_ptr< exportable_block_data > { return std::make_shared< exp_rc_data >(); } );
      }

      chain::database& db = appbase::app().get_plugin< steem::plugins::chain::chain_plugin >().db();

      my->_post_apply_block_conn = db.add_post_apply_block_handler( [&]( const block_notification& note ){ my->on_post_apply_block( note ); }, *this, 0 );
      my->_pre_apply_transaction_conn = db.add_pre_apply_transaction_handler( [&]( const transaction_notification& note ){ my->on_pre_apply_transaction( note ); }, *this, 0 );
      my->_post_apply_transaction_conn = db.add_post_apply_transaction_handler( [&]( const transaction_notification& note ){ my->on_post_apply_transaction( note ); }, *this, 0 );
      my->_pre_apply_operation_conn = db.add_pre_apply_operation_handler( [&]( const operation_notification& note ){ my->on_pre_apply_operation( note ); }, *this, 0 );
      my->_post_apply_operation_conn = db.add_post_apply_operation_handler( [&]( const operation_notification& note ){ my->on_post_apply_operation( note ); }, *this, 0 );

      add_plugin_index< rc_resource_param_index >(db);
      add_plugin_index< rc_pool_index >(db);
      add_plugin_index< rc_account_index >(db);
   }
   FC_CAPTURE_AND_RETHROW()
}

void rc_plugin::plugin_startup() {}

void rc_plugin::plugin_shutdown()
{
   chain::util::disconnect_signal( my->_post_apply_block_conn );
   chain::util::disconnect_signal( my->_post_apply_transaction_conn );
}

exp_rc_data::exp_rc_data() {}
exp_rc_data::~exp_rc_data() {}

void exp_rc_data::to_variant( fc::variant& v )const
{
   fc::to_variant( *this, v );
}

} } } // steem::plugins::rc
