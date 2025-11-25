# WebSearchLocation

Approximate location parameters for the search.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**country** | **str** | The two-letter  ISO country code of the user, e.g. &#x60;US&#x60;.  | [optional] 
**region** | **str** | Free text input for the region of the user, e.g. &#x60;California&#x60;.  | [optional] 
**city** | **str** | Free text input for the city of the user, e.g. &#x60;San Francisco&#x60;.  | [optional] 
**timezone** | **str** | The [IANA timezone] of the user, e.g. &#x60;America/Los_Angeles&#x60;.  | [optional] 

## Example

```python
from openapi_client.models.web_search_location import WebSearchLocation

# TODO update the JSON string below
json = "{}"
# create an instance of WebSearchLocation from a JSON string
web_search_location_instance = WebSearchLocation.from_json(json)
# print the JSON string representation of the object
print(WebSearchLocation.to_json())

# convert the object into a dict
web_search_location_dict = web_search_location_instance.to_dict()
# create an instance of WebSearchLocation from a dict
web_search_location_from_dict = WebSearchLocation.from_dict(web_search_location_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


