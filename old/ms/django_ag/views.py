'''

All of these views are predicated on the user already being logged in to
valid session.

djago_ag/views.py
John Whelchel
Summer 2013

These are the views for the Acquisition Gateway section of the administration
site. They are all decorated with @authenticate to make sure that a user is 
logged in; if not, they are redirected to the login page. Some are decorated
with precheck, a decorator that makes sure the passed g_id and passwords
are valid.

'''
import logging
import json

from django.http import HttpResponse
from django.shortcuts import redirect

from django.template import Context, loader, RequestContext
from django.views.decorators.csrf import csrf_exempt
from django.forms.formsets import formset_factory

from django_lib.auth import authenticate
from django_lib.decorators import precheck
from django_lib import gatewayforms
from django_lib import forms as libforms

from storage.storagetypes import transactional
import storage.storage as db

from MS.volume import Volume
from MS.user import SyndicateUser as User
from MS.gateway import AcquisitionGateway as AG


# This is the view to be redirected to when precheck fails; i.e.
# the given password or g_id is wrong.
PRECHECK_REDIRECT = 'django_ag.views.viewgateway'



@authenticate
def viewgateway(request, g_id=0):
    '''
    The view for viewing and changing any of the main settings on any AG. Passes
    forms for changing different settings, and the volumes attached to the gateway.
    '''
    session = request.session
    username = session['login_email']
    g_id = int(g_id)

    # Check for passed error messages or inital data from session-state.
    message = session.pop('message', "")
    ag_initial_data = session.get('ag_initial_data' + str(g_id), [])

    # Make sure this gateway actually exists.
    g = db.read_acquisition_gateway(g_id)
    if not g:
        logging.error("Error reading gateway %s : Does not exist." % (g_id))
        message = "No acquisition gateway with the ID %s exists." % g_id
        t = loader.get_template("gateway_templates/viewgateway_failure.html")
        c = Context({'message':message, 'username':username})
        return HttpResponse(t.render(c))

    # Create forms for changing location, adding volumes,
    # changing password, getting password, and changing config
    location_form = gatewayforms.ModifyGatewayLocation(initial={'host':g.host,
                                                                'port':g.port})
    add_form = gatewayforms.GatewayAddVolume()
    json_form = gatewayforms.ModifyGatewayConfig()
    password_form = libforms.Password()
    change_password_form = libforms.ChangePassword()

    # Get all attached volumes and their respective owners
    owners = []
    vols = []
    for v_id in g.volume_ids:
        vol = db.read_volume( v_id )
        if not vol:
            logging.error("Volume ID in gateways volume_ids does not map to volume. Gateway: %s" % g_name)
        else:
            vols.append(vol)
            attrs = {"SyndicateUser.owner_id ==":vol.owner_id}
            owners.append(db.get_user(attrs))
    vol_owners = zip(vols, owners)

    # Create formatted data based on vols for the formset, if not passed in state.
    if not ag_initial_data:
        for v in vols:
            ag_initial_data.append({'volume_name':v.name,
                                 'remove':False})
    session['ag_initial_data' + str(g_id)] = ag_initial_data

    VolumeFormSet = formset_factory(gatewayforms.GatewayRemoveVolume, extra=0)
    if ag_initial_data:
        formset = VolumeFormSet(initial=ag_initial_data)
    else:
        formset = None

    t = loader.get_template("gateway_templates/viewacquisitiongateway.html")
    c = RequestContext(request, {'username':username,
                        'gateway':g,
                        'message':message,
                        'vol_owners':vol_owners,
                        'location_form':location_form,
                        'add_form':add_form,
                        'json_form':json_form,
                        'remove_forms':formset,
                        'password_form':password_form,
                        'change_password_form':change_password_form})
    return HttpResponse(t.render(c))

@authenticate
@precheck("AG", PRECHECK_REDIRECT)
def changejson(request, g_id):
    '''
    Handler for changing json config file.
    '''
    session = request.session
    username = session['login_email']
    g_id = int(g_id)

    form = gatewayforms.ModifyGatewayConfig(request.POST)
    if form.is_valid():
        g = db.read_acquisition_gateway(g_id)
        if not g:
            session['message'] = "Gateway with ID {} does not exist.".format(g_id)
            return redirect('django_ag.views.viewgateway', g_id)

        # Verify upload success
        if 'json_config' not in request.FILES:
            session['message'] = "No uploaded file."
            return redirect('django_ag.views.viewgateway', g_id)
        if request.FILES['json_config'].multiple_chunks():
            session['message'] = "Uploaded file too large; please make smaller than 2.5M"
            return redirect('django_ag.views.viewgateway', g_id)
        config = request.FILES['json_config'].read()

        # Verify JSON format
        fields = {}
        try:
            fields['json_config'] = json.loads(config)
        except Exception as e:
            # File didn't read as JSON: try adding quotes on both sides just in case.
            logging.info("Possible JSON load error: %s" % e)
            try:
                fields['json_config'] = json.loads("\"" + config + "\"")
            except Exception as e:
                logging.error("Definite JSON load error %s" % e)
                session['message'] = "Error parsing given JSON text."
                return redirect('django_ag.views.viewgateway', g_id)

        # Update and redirect
        db.update_acquisition_gateway(g_id, **fields)
        session['new_change'] = "We've changed your gateways's JSON configuration."
        session['next_url'] = '/syn/AG/viewgateway/' + str(g_id)
        session['next_message'] = "Click here to go back to your gateway."
        return redirect('/syn/thanks')

    else:
        session['message'] = "Invalid form. Did you upload a file?"
        return redirect('django_ag.views.viewgateway', g_id)

# Doesn't use precheck() because doesn't use Password() form, just ChangePassword() form.
@authenticate
def changepassword(request, g_id):
    '''
    Handler for changing gateway password. Since it can't use precheck because of password reasons,
    must verify POST-ness itself.
    '''
    session = request.session
    username = session['login_email']
    g_id = int(g_id)


    # Precheck
    if request.method != "POST":
        return redirect('/syn/AG/viewgateway/' + str(g_id))

    try:
        g = db.read_acquisition_gateway(g_id)
        if not g:
            raise Exception("No gateway exists.")
    except Exception as e:
        logging.error("Error reading gateway with ID %d : Exception: %s" % (g_id, e))
        message = "No acquisition gateway with the ID %d exists." % g_id
        t = loader.get_template("gateway_templates/viewgateway_failure.html")
        c = Context({'message':message, 'username':username})
        return HttpResponse(t.render(c))



    form = libforms.ChangePassword(request.POST)

    if not form.is_valid():
        session['message'] = "You must fill out all password fields."
        return redirect('django_ag.views.viewgateway', g_id)
    else:
        # Check password hash
        if not AG.authenticate(g, form.cleaned_data['oldpassword']):
            session['message'] = "Incorrect password."
            return redirect('django_ag.views.viewgateway', g_id)
        elif form.cleaned_data['newpassword_1'] != form.cleaned_data['newpassword_2']:
            session['message'] = "Your new passwords did not match each other."
            return redirect('django_ag.views.viewgateway', g_id)
        # Ok to change password, then redirect
        else:
            new_hash = AG.generate_password_hash(form.cleaned_data['newpassword_1'])
            fields = {'ms_password_hash':new_hash}
            try:
                db.update_acquisition_gateway(g_id, **fields)
            except Exception as e:
                logging.error("Unable to update acquisition gateway %d. Exception %s" % (g_id, e))
                session['message'] = "Unable to update gateway."
                return redirect('django_ag.views.viewgateway', g_id)

            session['new_change'] = "We've changed your gateways's password."
            session['next_url'] = '/syn/AG/viewgateway/' + str(g_id)
            session['next_message'] = "Click here to go back to your gateway."
            return redirect('/syn/thanks')



@authenticate
@precheck("AG", PRECHECK_REDIRECT)
def addvolume(request, g_id):
    '''
    Handler for adding a volume to the gateay.
    '''

    # This is a helper method that isolates the @transactional decorator, speeding
    # up the code when it doesn't reach update() in this view and allowing for
    # errors that would break in GAE if the decorator was applied to the entire view.
    @transactional(xg=True)
    def update(v_id, g_id, vfields, gfields):
        db.update_volume(v_id, **vfields)
        db.update_acquisition_gateway(g_id, **gfields)
        session.pop('ag_initial_data' + str(g_id))

    session = request.session
    username = session['login_email']
    g_id = int(g_id)

    form = gatewayforms.GatewayAddVolume(request.POST)
    if form.is_valid():
        attrs = {"Volume.name ==":form.cleaned_data['volume_name'].strip().replace(" ", "_")}
        vols = db.list_volumes(attrs, limit=1)
        if vols:
            volume = vols[0]
            logging.info(volume)
        else:
            session['message'] = "The volume %s doesn't exist." % form.cleaned_data['volume_name']
            return redirect('django_ag.views.viewgateway', g_id)

        gateway = db.read_acquisition_gateway(g_id)

        # Prepare upcoming volume state
        if volume.ag_ids:
            new_ags = volume.ag_ids
            new_ags.append(gateway.g_id)
        else:
            new_ags = [gateway.g_id]
        vfields = {'ag_ids':new_ags}

        # Preare upcoming AG state
        old_vids = gateway.volume_ids
        new_vid = volume.volume_id
        if new_vid in old_vids:
            session['message'] = "That volume is already attached to this gateway!"
            return redirect('django_ag.views.viewgateway', g_id)
        if old_vids:
            old_vids.append(new_vid)
            new_vids = old_vids
        else:
            new_vids = [new_vid]

        # Update and redirect    
        try:
            gfields={'volume_ids':new_vids}
            update(volume.volume_id, g_id, vfields, gfields)
        except Exception as e:
            logging.error("Unable to update acquisition gateway %s or volume %s. Exception %s" % (gateway.ms_username, form.cleaned_data['volume_name'], e))
            session['message'] = "Unable to update gateway."
            return redirect('django_ag.views.viewgateway', g_id)
        session['new_change'] = "We've updated your AG's volumes."
        session['next_url'] = '/syn/AG/viewgateway/' + str(g_id)
        session['next_message'] = "Click here to go back to your gateway."
        return redirect('/syn/thanks')
    else:
        session['message'] = "Invalid entries for adding volumes."
        return redirect('django_ag.views.viewgateway', g_id)

@authenticate
@precheck("AG", PRECHECK_REDIRECT)
def removevolumes(request, g_id):
    '''
    This handler allows removal of one or many volumes from an Aqcquisition
    Gateway. It calls multi_update() as a helper method to allow transactional
    updates to the database.
    '''
    # This is a helper method that isolates the @transactional decorator, speeding
    # up the code when it doesn't reach update() in this view and allowing for
    # errors that would break in GAE if the decorator was applied to the entire view.
    # It updates multiple volumes at once
    @transactional(xg=True)
    def multi_update(v_ids, g_id, vfields, gfields):
        for v_id, vfield in zip(v_ids, vfields):
            db.update_volume(v_id, **vfield)
        db.update_acquisition_gateway(g_id, **gfields)
        session.pop('ag_initial_data' + str(g_id))

    session = request.session
    username = session['login_email']
    g_id = int(g_id)

    VolumeFormSet = formset_factory(gatewayforms.GatewayRemoveVolume, extra=0)
    formset = VolumeFormSet(request.POST)

    # This call is not checked because the formset will always be valid (readonly widgets)
    formset.is_valid()

    volume_ids_to_be_removed = []
    new_ags_set = []

    initial_and_forms = zip(session.get('ag_initial_data' + str(g_id), []), formset.forms)
    for i, f in initial_and_forms:

        if f.cleaned_data['remove']:
            attrs = {"Volume.name ==":i['volume_name']}
            vols = db.list_volumes(attrs, limit=1)
            vol = vols[0]

            # update each volume's new AG list
            new_ags = vol.ag_ids
            new_ags.remove(int(g_id))
            new_ags_set.append({'ag_ids':new_ags})

            # update info for AG update
            volume_ids_to_be_removed.append(vol.volume_id)

    if not volume_ids_to_be_removed:
        session['message'] = "You must select at least one volume to remove."
        return redirect('django_ag.views.viewgateway', g_id)

    old_vids = set(db.read_acquisition_gateway(g_id).volume_ids)
    new_vids = list(old_vids - set(volume_ids_to_be_removed))
    gfields = {'volume_ids':new_vids}
    try:
        multi_update(volume_ids_to_be_removed, g_id, new_ags_set, gfields)
    except Exception as e:
        logging.error("Unable to update acquisition gateway %s. Exception %s" % (g_id, e))
        session['message'] = "Unable to update gateway."
        return redirect('django_ag.views.viewgateway', g_id)
    session['new_change'] = "We've updated your AG's volumes."
    session['next_url'] = '/syn/AG/viewgateway/' + str(g_id)
    session['next_message'] = "Click here to go back to your gateway."
    return redirect('/syn/thanks')


@authenticate
@precheck("AG", PRECHECK_REDIRECT)
def changelocation(request, g_id):
    '''
    Handler for changing the host:port of the gateway.
    '''
    session = request.session
    username = session['login_email']
    g_id = int(g_id)
        

    form = gatewayforms.ModifyGatewayLocation(request.POST)
    if form.is_valid():
        new_host = form.cleaned_data['host']
        new_port = form.cleaned_data['port']
        fields = {'host':new_host, 'port':new_port}

        # Update and redirect
        try:
            db.update_acquisition_gateway(g_id, **fields)
        except Exception as e:
            logging.error("Unable to update AG: %d. Error was %s." % (g_id, e))
            session['message'] = "Error. Unable to change acquisition gateway."
            return redirect('django_ag.views.viewgateway', g_id)

        session['new_change'] = "We've updated your AG."
        session['next_url'] = '/syn/AG/viewgateway/' + str(g_id)
        session['next_message'] = "Click here to go back to your gateway."
        return redirect('/syn/thanks')
    else:
        session['message'] = "Invalid form entries for gateway location."
        return redirect('django_ag.views.viewgateway', g_id)

@authenticate
def allgateways(request):
    '''
    View to look at all AG's in a tabular format, with owners and attached volumes.
    '''
    session = request.session
    username = session['login_email']

    # Get gateways
    try:
        qry = db.list_acquisition_gateways()
    except:
        qry = []
    gateways = []
    for g in qry:
        gateways.append(g)

    # Get volumes and owners
    vols = []
    g_owners = []
    for g in gateways:
        volset = []
        for v in g.volume_ids:
            volset.append(db.read_volume(v))
        vols.append(volset)
        attrs = {"SyndicateUser.owner_id ==":g.owner_id}
        g_owners.append(db.get_user(attrs))


    gateway_vols_owners = zip(gateways, vols, g_owners)
    t = loader.get_template('gateway_templates/allacquisitiongateways.html')
    c = RequestContext(request, {'username':username, 'gateway_vols_owners':gateway_vols_owners})
    return HttpResponse(t.render(c))

@authenticate
def create(request):
    '''
    View to handle creation of AG's
    '''
    session = request.session
    username = session['login_email']
    user = db.read_user( username )

    # Helper method used to simplify error-handling. When fields are entered incorrectly,
    # a session message is set and this method is called.
    def give_create_form(username, session):
        message = session.pop('message', "")
        form = gatewayforms.CreateAG()
        t = loader.get_template('gateway_templates/create_acquisition_gateway.html')
        c = RequestContext(request, {'username':username,'form':form, 'message':message})
        return HttpResponse(t.render(c))

    if request.POST:
        # Validate input forms
        form = gatewayforms.CreateAG(request.POST, request.FILES)
        if form.is_valid():
            kwargs = {}

            # Try and load JSON config file/data, if present. First check uploaded files, then
            # the text box.
            if "json_config" in request.FILES:
                if request.FILES['json_config'].multiple_chunks():
                    session['message'] = "Uploaded file too large; please make smaller than 2.5M"
                    return give_create_form(username, session)
                config = request.FILES['json_config'].read()
                try:
                    kwargs['json_config'] = json.loads(config)
                except Exception as e:
                    logging.info("Possible JSON load error: %s" % e)
                    try:
                        kwargs['json_config'] = json.loads("\"" + config + "\"")
                    except Exception as e:
                        logging.error("Definite JSON load error %s" % e)
                        session['message'] = "Error parsing given JSON text."
                        return give_create_form(username, session)

            # No upload, check text box.
            elif "json_config_text" in form.cleaned_data:
                try:
                    kwargs['json_config'] = json.loads(form.cleaned_data['json_config_text'])
                except Exception as e:
                    logging.info("Possible JSON load error: %s" % e)
                    try:
                        kwargs['json_config'] = json.loads("\"" + str(form.cleaned_data['json_config_text']) + "\"")
                    except Exception as e:
                        logging.error("Definite JSON load error %s" % e)
                        session['message'] = "Error parsing given JSON text."
                        return give_create_form(username, session)


            try:
                kwargs['ms_username'] = form.cleaned_data['g_name']
                kwargs['port'] = form.cleaned_data['port']
                kwargs['host'] = form.cleaned_data['host']
                kwargs['ms_password'] = form.cleaned_data['g_password']
                new_ag = db.create_acquisition_gateway(user, **kwargs)
            except Exception as E:
                session['message'] = "AG creation error: %s" % E
                return give_create_form(username, session)

            session['new_change'] = "Your new gateway is ready."
            session['next_url'] = '/syn/AG/allgateways'
            session['next_message'] = "Click here to see your acquisition gateways."
            return redirect('/syn/thanks/')
        else:
            # Prep returned form values (so they don't have to re-enter stuff)

            if 'g_name' in form.errors:
                oldname = ""
            else:
                oldname = request.POST['g_name']
            if 'host' in form.errors:
                oldhost = ""
            else:
                oldhost = request.POST['host']
            if 'port' in form.errors:
                oldport = ""
            else:
                oldport = request.POST['port']
            oldjson = request.POST['json_config_text']
            # Prep error message
            message = "Invalid form entry: "

            for k, v in form.errors.items():
                message = message + "\"" + k + "\"" + " -> " 
                for m in v:
                    message = message + m + " "

            # Give them the form again
            form = gatewayforms.CreateAG(initial={'g_name': oldname,
                                       'host': oldhost,
                                       'port': oldport,
                                       'json_config_text':oldjson,
                                       })
            t = loader.get_template('gateway_templates/create_acquisition_gateway.html')
            c = RequestContext(request, {'username':username,'form':form, 'message':message})
            return HttpResponse(t.render(c))

    else:
        # Not a POST, give them blank form
        return give_create_form(username, session)

@authenticate
def delete(request, g_id):
    '''
    View for deleting AG.
    '''

    # Helper method used to simplify error-handling. When fields are entered incorrectly,
    # a session message is set and this method is called.
    def give_delete_form(username, g, session):
        message = session.pop('message', "")
        form = gatewayforms.DeleteGateway()
        t = loader.get_template('gateway_templates/delete_acquisition_gateway.html')
        c = RequestContext(request, {'username':username, 'g':g, 'form':form, 'message':message})
        return HttpResponse(t.render(c))

    # Once again isolating transactional for views that update multiple entities
    @transactional(xg=True)
    def delete_and_update(ag_id, attached_volume_ids):
        db.delete_acquisition_gateway(ag_id)
        for v in attached_volume_ids:
            vol = db.read_volume(v)
            if not vol:
                continue
            new_ag_ids = vol.ag_ids
            new_ag_ids.remove(ag_id)
            attrs = {"ag_ids":new_ag_ids}
            db.update_volume(v, **attrs)
        session.pop("ag_initial_data", None)

    session = request.session
    username = session['login_email']
    g_id = int(g_id)

    ag = db.read_acquisition_gateway(g_id)
    if not ag:
        t = loader.get_template('gateway_templates/delete_acquisition_gateway_failure.html')
        c = RequestContext(request, {'username':username})
        return HttpResponse(t.render(c))

    if request.POST:
        # Validate input forms
        form = gatewayforms.DeleteGateway(request.POST)
        if form.is_valid():
            if not AG.authenticate(ag, form.cleaned_data['g_password']):
                session['message'] = "Incorrect Acquisition Gateway password"
                return give_delete_form(username, ag, session)
            if not form.cleaned_data['confirm_delete']:
                session['message'] = "You must tick the delete confirmation box."
                return give_delete_form(username, ag, session)
            
            delete_and_update(g_id, ag.volume_ids)
            session['new_change'] = "Your gateway has been deleted."
            session['next_url'] = '/syn/AG/allgateways'
            session['next_message'] = "Click here to see all acquisition gateways."
            return redirect('/syn/thanks/')

        # Invalid forms
        else:  
            # Prep error message
            session['message'] = "Invalid form entry: "

            for k, v in form.errors.items():
                session['message'] = session['message'] + "\"" + k + "\"" + " -> " 
                for m in v:
                    session['message'] = session['message'] + m + " "

            return give_delete_form(username, ag, session)
    else:
        # Not a POST, give them blank form
        return give_delete_form(username, ag, session)


@csrf_exempt
@authenticate
def urlcreate(request, g_name, g_password, host, port, volume_name="",):
    '''
    For debugging purposes only, allows creation of AG via pure URL
    '''
    session = request.session
    username = session['login_email']

    kwargs = {}

    kwargs['port'] = int(port)
    kwargs['host'] = host
    kwargs['ms_username'] = g_name
    kwargs['ms_password'] = g_password
    if volume_name:
        vol = db.get_volume_by_name(volume_name)
        if not vol:
            return HttpResponse("No volume %s exists." % volume_name)
        if (vol.volume_id not in user.volumes_r) and (vol.volume_id not in user.volumes_rw):
            return HttpResponse("Must have read rights to volume %s to create AG for it." % volume_name)
        kwargs['volume_ids'] = [vol.volume_id]

    try:
        new_ag = db.create_acquisition_gateway(user, **kwargs)
    except Exception as E:
        return HttpResponse("AG creation error: %s" % E)

    return HttpResponse("AG succesfully created: " + str(new_ag))

@csrf_exempt
@authenticate
def urldelete(request, g_name, g_password):
    '''
    For debugging purposes only, allows deletion of AG via pure URL
    '''
    session = request.session
    username = session['login_email']

    attrs = {"AcquisitionGateway.ms_username ==":g_name}
    ags = db.list_acquisition_gateways(attrs, limit=1)
    if ags:
        ag = ags[0]
    else:
        return HttpResponse("AG %s does not exist." % g_name)
    if not AG.authenticate(ag, g_password):
        return HttpResponse("Incorrect AG password.")
    db.delete_acquisition_gateway(g_name)
    return HttpResponse("Gateway succesfully deleted.")
